#pragma once

#include <curl/curl.h>
#include <cstdio>
#include <string>

size_t write_file(void* data, size_t size, size_t count, void* file) {
    return std::fwrite(data, size, count, static_cast<FILE*>(file));
}

bool curl_download(const std::string& url, const std::string& output) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    FILE* file = std::fopen(output.c_str(), "wb");
    if (!file) {
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode result = curl_easy_perform(curl);

    std::fclose(file);
    curl_easy_cleanup(curl);

    return result == CURLE_OK;
}
