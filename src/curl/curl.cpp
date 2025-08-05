/* <editor-fold desc="MIT License">

Copyright(c) 2021 Robert Osfield

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

</editor-fold> */

#include <vsg/io/read.h>
#include <vsgXchange/curl.h>

#include <curl/curl.h>

#include <fstream>
#include <iostream>
#include <sstream>

using namespace vsgXchange;

namespace vsgXchange
{

    bool containsServerAddress(const vsg::Path& filename)
    {
        return filename.compare(0, 7, "http://") == 0 || filename.compare(0, 8, "https://") == 0;
    }

    std::pair<vsg::Path, vsg::Path> getServerPathAndFilename(const vsg::Path& filename)
    {
        auto pos = filename.find("://");
        if (pos != vsg::Path::npos)
        {
            auto pos_slash = filename.find_first_of('/', pos + 3);
            if (pos_slash != vsg::Path::npos)
            {
                return {filename.substr(pos + 3, pos_slash - pos - 3), filename.substr(pos_slash + 1, vsg::Path::npos)};
            }
            else
            {
                return {filename.substr(pos + 3, vsg::Path::npos), ""};
            }
        }
        return {};
    }

    std::string extractValue(const std::string& str, const std::string& key)
    {
        auto pos = str.find(key);
        if (pos != std::string::npos)
        {
            pos += key.length();
            auto end_pos = str.find('&', pos);
            if (end_pos == std::string::npos) end_pos = str.length();
            return str.substr(pos, end_pos - pos);
        }
        return {};
    }

    vsg::Path getFileCacheNameGoogle(const vsg::Path& filename)
    {
        std::string str = filename.string();
        auto start = str.find("://");
        auto end = str.rfind('/');

        vsg::Path filepath = str.substr(start + 3, end - start - 3); // Extract the part after "://"

        auto lyrs = extractValue(str, "lyrs=");
        auto z = extractValue(str, "z=");
        auto x = extractValue(str, "x=");
        auto y = extractValue(str, "y=");

        x += ".png";

        filepath = filepath / lyrs / z / y / x;

        return filepath;
    }

    vsg::Path getFileCachePath(const vsg::Path& fileCache, const vsg::Path& filename)
    {
        std::string filepath;

        auto str = filename.string();
        auto lyrs_pos = str.find("google");
        if(lyrs_pos != std::string::npos)
        {
            filepath = getFileCacheNameGoogle(filename);
        }
        else
        {
            auto pos = filename.find("://");
            if (pos != vsg::Path::npos)
            {
                filepath = filename.substr(pos + 3, vsg::Path::npos);
            }
        }

        return fileCache / filepath;
    }

    class curl::Implementation
    {
    public:
        Implementation();
        virtual ~Implementation();

        vsg::ref_ptr<vsg::Object> read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options = {}) const;

    protected:
    };

} // namespace vsgXchange

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// CURL ReaderWriter facade
//
curl::curl() :
    _implementation(nullptr)
{
}
curl::~curl()
{
    delete _implementation;
}
vsg::ref_ptr<vsg::Object> curl::read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
{
    vsg::Path serverFilename = filename;

    bool contains_serverAddress = containsServerAddress(filename);

    if (options)
    {
        if (!contains_serverAddress && !options->paths.empty())
        {
            contains_serverAddress = containsServerAddress(options->paths.front());
            if (contains_serverAddress)
            {
                serverFilename = options->paths.front() / filename;
            }
        }

        if (contains_serverAddress && options->fileCache)
        {
            auto fileCachePath = getFileCachePath(options->fileCache, serverFilename);
            if (vsg::fileExists(fileCachePath))
            {
                auto local_options = vsg::clone(options);

                local_options->paths.insert(local_options->paths.begin(), vsg::filePath(fileCachePath));
                local_options->extensionHint = vsg::lowerCaseFileExtension(filename);

                // std::ifstream fin(fileCachePath, std::ios::in | std::ios::binary);
                auto object = vsg::read(fileCachePath, local_options); // do we need to remove any http URL?
                if (object)
                {
                    return object;
                }
            }
        }
    }

    if (contains_serverAddress)
    {
        {
            std::scoped_lock<std::mutex> lock(_mutex);
            if (!_implementation) _implementation = new curl::Implementation();
        }

        return _implementation->read(serverFilename, options);
    }
    else
    {
        return {};
    }
}

bool curl::getFeatures(Features& features) const
{
    features.protocolFeatureMap["http"] = vsg::ReaderWriter::READ_FILENAME;
    features.protocolFeatureMap["https"] = vsg::ReaderWriter::READ_FILENAME;
    features.optionNameTypeMap[curl::SSL_OPTIONS] = "uint32_t";
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// CURL ReaderWriter implementation
//

// use static mutex and counter to track whether the curl_global_init(..) and curl_global_cleanup() should be called.
bool curl::s_do_curl_global_init_and_cleanup = true;
std::mutex s_curlImplementationMutex;
uint32_t s_curlImplementationCount = 0;

curl::Implementation::Implementation()
{
    if (curl::s_do_curl_global_init_and_cleanup)
    {
        std::scoped_lock<std::mutex> lock(s_curlImplementationMutex);
        if (s_curlImplementationCount == 0)
        {
            //std::cout<<"curl_global_init()"<<std::endl;
            curl_global_init(CURL_GLOBAL_ALL);
        }
        ++s_curlImplementationCount;
    }
}

curl::Implementation::~Implementation()
{
    if (s_do_curl_global_init_and_cleanup)
    {
        std::scoped_lock<std::mutex> lock(s_curlImplementationMutex);
        --s_curlImplementationCount;
        if (s_curlImplementationCount == 0)
        {
            //std::cout<<"curl_global_cleanup()"<<std::endl;
            curl_global_cleanup();
        }
    }
}

size_t StreamCallback(void* ptr, size_t size, size_t nmemb, void* user_data)
{
    size_t realsize = size * nmemb;

    if (user_data)
    {
        std::ostream* ostr = reinterpret_cast<std::ostream*>(user_data);
        ostr->write(reinterpret_cast<const char*>(ptr), realsize);
    }

    return realsize;
}

vsg::ref_ptr<vsg::Object> curl::Implementation::read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
{
    auto _curl = curl_easy_init();

    curl_easy_setopt(_curl, CURLOPT_USERAGENT, "libcurl-agent/1.0"); // make user controllable?
    curl_easy_setopt(_curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, StreamCallback);

    std::stringstream sstr;

    curl_easy_setopt(_curl, CURLOPT_URL, filename.string().c_str());
    curl_easy_setopt(_curl, CURLOPT_WRITEDATA, (void*)&sstr);

    uint32_t sslOptions = 0;
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
    sslOptions |= CURLSSLOPT_NATIVE_CA;
#endif
    if (options) options->getValue(curl::SSL_OPTIONS, sslOptions);
    if (sslOptions != 0) curl_easy_setopt(_curl, CURLOPT_SSL_OPTIONS, sslOptions);

    vsg::ref_ptr<vsg::Object> object;

    CURLcode result = curl_easy_perform(_curl);
    if (result == 0)
    {
        // https://developer.mozilla.org/en-US/docs/Web/HTTP/Status
        long response_code = 0;
        result = curl_easy_getinfo(_curl, CURLINFO_RESPONSE_CODE, &response_code);

        if (result == 0 && response_code >= 200 && response_code < 300) // successful responses.
        {
            // success
            auto local_options = vsg::clone(options);
            local_options->paths.insert(local_options->paths.begin(), vsg::filePath(filename));
            if (!local_options->extensionHint)
            {
                local_options->extensionHint = vsg::lowerCaseFileExtension(filename);
            }

            object = vsg::read(sstr, local_options);

            if (object && options->fileCache)
            {
                auto fileCachePath = getFileCachePath(options->fileCache, filename);
                if (fileCachePath)
                {
                    vsg::makeDirectory(vsg::filePath(fileCachePath));

                    // reset the stringstream iterator to the beginning so we can copy it to the file cache file.
                    sstr.clear(std::stringstream::goodbit);
                    sstr.seekg(0);

                    std::ofstream fout(fileCachePath, std::ios::out | std::ios::binary);

                    fout << sstr.rdbuf();
                }
            }
        }
        else
        {
            object = vsg::ReadError::create(vsg::make_string("vsgXchange::curl could not read file ", filename, ", CURLINFO_RESPONSE_CODE = ", response_code));
        }
    }
    else
    {
        object = vsg::ReadError::create(vsg::make_string("vsgXchange::curl could not read file ", filename, ", result = ", result, ", ", curl_easy_strerror(result)));
    }

    if (_curl) curl_easy_cleanup(_curl);

    return object;
}
