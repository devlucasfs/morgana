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

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Cache-Control: no-cache, no-store");
    headers = curl_slist_append(headers, "Pragma: no-cache");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);

    CURLcode result = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    std::fclose(file);
    curl_easy_cleanup(curl);

    return result == CURLE_OK;
}
