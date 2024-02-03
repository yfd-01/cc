/*
    基于curl的多线程下载
*/

#include <cstdio>       // snprintf
#include <curl/curl.h>
#include <curl/easy.h>
#include <iostream>
#include <fcntl.h>      // open
#include <unistd.h>     // lseek
#include <string.h>     // memcpy
#include <sys/mman.h>   // mmap
#include <vector>
#include <thread>
#include <mutex>

// https://releases.ubuntu.com/jammy/ubuntu-22.04.3-desktop-amd64.iso.zsync

struct FilePartition {
    void* ptr;
    size_t start;
    size_t offset;
    size_t end;
    size_t gl_len;

    FilePartition(void* ptr, size_t start, size_t end, size_t len)
        : ptr(ptr), start(start), offset(start), end(end), gl_len(len) {}
};

using FilePartitionStore = std::vector<FilePartition*>;

size_t write_func(void* ptr, size_t size, size_t memb, void* usr_data) {
    // std::cout<< std::this_thread::get_id();
    // printf(" - offset(%ld) - end(%ld)\n", file->offset, file->end);

    FilePartition* file = static_cast<FilePartition*>(usr_data);

    memcpy((char*)file->ptr + file->offset, ptr, size * memb);
    file->offset += size * memb;

    return size * memb;
}

int progress_func(void* usr_data, double dl_total, double dl_now, double up_total, double up_now) {
    static short percent = 0;
    FilePartitionStore indicatiors = *static_cast<FilePartitionStore*>(usr_data);

    size_t downloaded = 0;
    for (auto file : indicatiors)
        downloaded += file->offset - file->start;
    
    short _percent = downloaded * 100 / indicatiors[0]->gl_len;

    if (percent != _percent) {
        percent = _percent;
        std::cout<< "downloaded: "<< percent<< "%\n";
    }

    return 0;
}

size_t _slience_cb(void*, size_t size, size_t memb, void*) {
    return size * memb;
};

// 获取下载内容长度
size_t get_download_file_length(const char* url) {
    CURL* curl = curl_easy_init();

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HEADER, 1L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _slience_cb);

    size_t len = 0;

    if (curl_easy_perform(curl) == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);

    curl_easy_cleanup(curl);
    
    return len;
}

// 分片
std::vector<FilePartition*> prepare_partition(size_t len, short thread_nums, void* ptr) {
    size_t base_range = len / thread_nums;
    
    FilePartitionStore indicatiors;
    indicatiors.reserve(thread_nums);

    for (int i = 0; i < thread_nums; ++i) {
        if (i == thread_nums - 1)
            indicatiors.emplace_back(new FilePartition(ptr, base_range * i, len - 1, len));
        else
            indicatiors.emplace_back(new FilePartition(ptr, base_range * i, base_range * (i + 1) - 1, len));
    }

    return indicatiors;     // vec指针元素，无太多拷贝代价
}

// 初始化同大小文件并将其映射至内存
void* crt_local_file_mapping(size_t len, const char* filename, int& fd) {
    fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);

    if (fd == -1) {
        std::cerr<< "crt file failed\n";
        return nullptr;
    }

    if (lseek(fd, len - 1, SEEK_SET) == -1) {
        std::cerr<< "set file seek failed\n";
        close(fd);
        return nullptr;
    }

    if (write(fd, "", 1) != 1) {
        std::cerr<< "file init failed\n";
        close(fd);
        return nullptr;
    }

    void* ptr = mmap(NULL, len, PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        std::cerr<< "map file failed\n";
        close(fd);
        return nullptr;
    }

    return ptr;
}

/**
*   思路过程：
*   1.  获取下载文件的内容大小
*   2.  在本地磁盘创建同样大小的文件（open-lseek-write或者open-ftruncate)
*   3.  mmap将该文件映射至内存区块
*   4.  根据线程数量进行分区，使每个线程承担部分内容的下载任务（用结构体指针引导线程的回写位置）
*   5.  线程完成后加锁统计写入结果，用于判断是否所有区块均已完成
*/
int download(const char* url, const char* filename, short thread_nums) {
    size_t len = get_download_file_length(url);

    int fd;
    void* ptr = crt_local_file_mapping(len, filename, fd);

    if (ptr == nullptr)
        return 0;
    
    FilePartitionStore partitions = prepare_partition(len, thread_nums, ptr);
    std::vector<std::thread> ths;
    std::mutex mtx;
    short cnt = 0;

    for (int i = 0; i < thread_nums; i++) {
        ths.emplace_back([&partitions, url, i, &mtx, &cnt]() {
            char range_buff[64] = { 0 };
            snprintf(range_buff, 64, "%ld-%ld", partitions[i]->offset, partitions[i]->end);
            std::cout<< "thread["<<std::this_thread::get_id()<< "] downloads the file contents in the range of "<< range_buff<< '\n';

            CURL* curl = curl_easy_init();

            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_func);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, partitions[i]);
            curl_easy_setopt(curl, CURLOPT_RANGE, range_buff);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_func);
            curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, &partitions);

            CURLcode res = curl_easy_perform(curl);

            curl_easy_cleanup(curl);

            std::lock_guard<std::mutex> lg(mtx);
            cnt += res == CURLE_OK;
        });
    }

    for (auto& th : ths)
        th.join();
    
    close(fd);
    munmap(ptr, len);

    for (auto p : partitions)
        delete p;
    
    return cnt == thread_nums;
}

int main(int argc, const char* argv[]) {
    if (argc != 4)  return -1;

    auto res = download(argv[1], argv[2], std::atoi(argv[3]));
    std::cout<< "download result: "<< res<< '\n';

    return 0;
}
