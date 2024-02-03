/*
    基于curl的单线程下载
*/

#include <curl/curl.h>
#include <curl/easy.h>
#include <iostream>
#include <memory>
#include <fcntl.h>      // open
#include <unistd.h>     // lseek
#include <string.h>     // memcpy
#include <sys/mman.h>   // mmap

// https://releases.ubuntu.com/jammy/ubuntu-22.04.3-desktop-amd64.iso.zsync

struct FileOpener {
    void* ptr;
    size_t offset;

    ~FileOpener() {
        std::cout<< "fo released\n";
    }
};

size_t write_func(void* ptr, size_t size, size_t memb, void* param) {
    FileOpener* file = static_cast<FileOpener*>(param);
    std::cout<< "file downloaded: "<< file->offset<< '\n';

    memcpy((char*)file->ptr + file->offset, ptr, size * memb);
    file->offset += size * memb;

    return size * memb;
}

size_t get_download_file_length(const char* url) {
    CURL* curl = curl_easy_init();

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HEADER, 0);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1);

    size_t len = 0;

    if (curl_easy_perform(curl) == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);

    curl_easy_cleanup(curl);
    
    return len;
}

int download(const char* url, const char* filename) {
    size_t len = get_download_file_length(url);

    int fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        std::cerr<< "crt file failed\n";
        return 0;
    }

    if (lseek(fd, len - 1, SEEK_SET) == -1) {
        std::cerr<< "set file seek failed\n";
        close(fd);
        return 0;
    }

    if (write(fd, "", 1) != 1) {
        std::cerr<< "file init failed\n";
        close(fd);
        return 0;
    }

    void* ptr = mmap(NULL, len, PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        std::cerr<< "map file failed\n";
        close(fd);
        return 0;
    }

    CURL* curl = curl_easy_init();
    std::unique_ptr<FileOpener> fo(new FileOpener());
    fo->ptr = ptr;
    fo->offset = 0;
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_func);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fo.get());

    CURLcode res = curl_easy_perform(curl);

    curl_easy_cleanup(curl);
    close(fd);
    munmap(ptr, len);

    return res == CURLE_OK;
}

int main(int argc, const char* argv[]) {
    if (argc != 3)  return -1;

    download(argv[1], argv[2]);

    return 0;
}
