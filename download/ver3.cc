/*
    基于curl的多线程下载，断点续传版
*/

#include <curl/curl.h>
#include <curl/easy.h>
#include <cstdio>       // snprintf
#include <fcntl.h>      // open
#include <unistd.h>     // lseek    fsync
#include <string.h>     // memcpy
#include <sys/mman.h>   // mmap
#include <signal.h>     // signal
#include <vector>
#include <thread>
#include <mutex>
#include <iostream>

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

// 定义为全局变量，使其在信号处理函数中使用
FilePartitionStore partitions;
int file_fd = -1;
const char* RESUME_FILENAME = ".bp_resume";

// 写入回调
size_t write_func(void* ptr, size_t size, size_t memb, void* usr_data) {
    // std::cout<< std::this_thread::get_id();
    // printf(" - offset(%ld) - end(%ld)\n", file->offset, file->end);

    FilePartition* file = static_cast<FilePartition*>(usr_data);
    memcpy((char*)file->ptr + file->offset, ptr, size * memb);
    file->offset += size * memb;

    return size * memb;
}

// 进度条渲染
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
    FILE* fp = fopen(RESUME_FILENAME, "r");
    bool resume_flag = false;

    if (fp) {
        short last_thread_nums;
        fscanf(fp, "[%hd]", &last_thread_nums);

        resume_flag = last_thread_nums == thread_nums;
    }
    
    FilePartitionStore indicatiors;
    indicatiors.reserve(thread_nums);

    for (int i = 0; i < thread_nums; ++i) {
        if (!resume_flag) {
            size_t base_range = len / thread_nums;

            if (i == thread_nums - 1)
                indicatiors.emplace_back(new FilePartition(ptr, base_range * i, len - 1, len));
            else
                indicatiors.emplace_back(new FilePartition(ptr, base_range * i, base_range * (i + 1) - 1, len));
        } else {
            size_t offset, end;
            fscanf(fp, "%ld-%ld", &offset, &end);

            indicatiors.emplace_back(new FilePartition(ptr, offset, end, len));
        }
    }

    return indicatiors;     // vec指针元素，无太多拷贝代价
}

// 初始化同大小文件并将其映射至内存
void* crt_local_file_mapping(size_t len, const char* filename, int& fd) {
    fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);

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

    file_fd = fd;

    return ptr;
}

/**
*   思路过程：
*   1.  捕获中断信号
*   2.  信号处理函数中将每个分片的offset和end写入文件
    👇使得
*   3.  在程序最初构造分片集合时，若存在断点信息文件，则直接从文件中获取每个分片的上次写入情况
*   4.  在子线程运行时，若offset>end则说明该分片内容写入完成，该线程不进行资源请求
*   5.  注意在信号处理函数中，需要将mmap内存数据刷入磁盘（write或fsync），且应该在写入断点信息文件后做该操作。否则可能存在offset（刷入磁盘时）到offset（写入文件）间的数据内容丢失，因为子线程不断在前推offset值
*   
*   优化点：
*   1.  根据待请求的数量构造线程数量，即offset<=end的数量
*   2.  进度条分割，例如运行三次完成该文件，出现20%、60%、20%
*   3.  将请求资源与断点信息文件进行绑定，如用hash后缀命名
*/
int download(const char* url, const char* filename, short thread_nums) {
    size_t len = get_download_file_length(url);

    int fd;
    void* ptr = crt_local_file_mapping(len, filename, fd);

    if (ptr == nullptr)
        return 0;
    
    partitions = prepare_partition(len, thread_nums, ptr);
    std::vector<std::thread> ths;
    std::mutex mtx;
    short cnt = 0;

    for (int i = 0; i < thread_nums; i++) {
        ths.emplace_back([url, i, &mtx, &cnt]() {
            // 断点前完成
            if (partitions[i]->offset > partitions[i]->end) {
                std::lock_guard<std::mutex> lg(mtx);
                cnt++;
                return;
            }

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

// 中断信号处理
void signal_handler(int sig_num) {
    if (partitions.size() == 0) exit(1);
    std::cout<< "downloadeding point saved\n";

    // 文件断点信息保存
    int fd = open(RESUME_FILENAME, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    
    char buff[64] = {0};
    snprintf(buff, 64, "[%ld]\r\n", partitions.size());
    write(fd, buff, strlen(buff));

    for (auto partition : partitions) {
        snprintf(buff, 64, "%ld-%ld\r\n", partition->offset, partition->end);

        write(fd, buff, strlen(buff));
    }
    
    close(fd);

    if (file_fd != -1)
        close(file_fd);     // 文件内容刷入磁盘

    exit(1);
}

int main(int argc, const char* argv[]) {
    if (argc != 4)  return -1;
    if (signal(SIGINT, signal_handler) == SIG_ERR) {
        std::cerr<< "signal error: "<< strerror(errno)<< '\n';
        return -1;
    }

    auto res = download(argv[1], argv[2], std::atoi(argv[3]));
    std::cout<< "download result: "<< res<< '\n';

    return 0;
}
