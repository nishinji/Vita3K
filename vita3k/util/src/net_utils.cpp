// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <util/log.h>
#include <util/net_utils.h>

#include <curl/curl.h>

#ifdef _WIN32
#include <iphlpapi.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <iterator>
#include <memory>
#include <mutex>
#include <ranges>
#include <utility>

namespace net_utils {

using namespace std::string_view_literals;

static bool is_digit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

// 0 is ok, negative is bad
SceHttpErrorCode parse_url(const std::string &url, parsedUrl &out) {
    out.scheme = url.substr(0, url.find(':'));

    if (out.scheme != "http" && out.scheme != "https")
        return SCE_HTTP_ERROR_UNKNOWN_SCHEME;

    // An opaque URL (no "://" after the scheme) is invalid
    if (!std::string_view(url).substr(out.scheme.length()).starts_with("://")) {
        out.invalid = true;
        return SceHttpErrorCode{};
    }

    auto end_scheme_pos = url.find(':');
    auto has_path = url.find('/', end_scheme_pos + 3) != std::string::npos;

    auto full_wo_scheme = url.substr(end_scheme_pos + 3);

    if (has_path) {
        // username:password@lttstore.com:727/wysi/cookie.php?pog=gers#extremeexploit

        {
            auto path_pos = full_wo_scheme.find('/');
            auto full_no_scheme_path = full_wo_scheme.substr(0, path_pos);
            // full_no_scheme_path = username:password@lttstore.com:727
            auto c = full_no_scheme_path.find('@');
            if (c != std::string::npos) { // we have credentials
                auto credentials = full_no_scheme_path.substr(0, c);
                // credentials = username:password
                auto semicolon_pos = credentials.find(':');

                if (semicolon_pos != std::string::npos) { // we have password?
                    auto username = credentials.substr(0, semicolon_pos);
                    auto password = credentials.substr(semicolon_pos + 1);

                    if (username.length() >= SCE_HTTP_USERNAME_MAX_SIZE)
                        return SCE_HTTP_ERROR_OUT_OF_SIZE;
                    if (password.length() >= SCE_HTTP_USERNAME_MAX_SIZE)
                        return SCE_HTTP_ERROR_OUT_OF_SIZE;

                    out.username = username;
                    out.password = password;
                } else { // we don't have password
                    out.username = credentials;
                }
            } else { // no credentials
                // lttstore.com:727
                auto semicolon_pos = full_no_scheme_path.find(':');

                if (semicolon_pos != std::string::npos) { // we have port
                    auto hostname = full_no_scheme_path.substr(0, semicolon_pos);
                    auto port = full_no_scheme_path.substr(semicolon_pos + 1);

                    out.hostname = hostname;
                    out.port = port;
                } else { // no port
                    out.hostname = full_no_scheme_path;
                }
            }
        }
        {
            auto path_pos = full_wo_scheme.find('/');
            auto full_no_scheme_hostname = full_wo_scheme.substr(path_pos);
            // /wysi/cookie.php?pog=gers#extremeexploit

            auto query_pos = full_no_scheme_hostname.find('?');
            if (query_pos != std::string::npos) { // we have query
                auto path = full_no_scheme_hostname.substr(0, query_pos);
                out.path = path;

                auto query_frag = full_no_scheme_hostname.substr(query_pos);

                auto frag_pos = query_frag.find('#');
                if (frag_pos != std::string::npos) {
                    // query and fragment

                    auto query = query_frag.substr(0, frag_pos);
                    auto fragment = query_frag.substr(frag_pos);

                    out.query = query;
                    out.fragment = fragment;
                } else { // no fragment
                    out.query = query_frag;
                }

            } else { // no query, dunno about fragment
                auto frag_pos = full_no_scheme_hostname.find('#');

                if (frag_pos != std::string::npos) { // we have fragment
                    auto path = full_no_scheme_hostname.substr(0, frag_pos);
                    auto fragment = full_no_scheme_hostname.substr(frag_pos);

                    out.path = path;
                    out.fragment = fragment;
                } else { // no fragment, only path
                    out.path = full_no_scheme_hostname;
                }
            }
        }
    } else {
        // username:password@lttstore.com:727
        auto c = full_wo_scheme.find('@');
        if (c != std::string::npos) { // we have credentials
            auto credentials = full_wo_scheme.substr(0, c);
            // credentials = username:password
            auto semicolon_pos = credentials.find(':');

            if (semicolon_pos != std::string::npos) { // we have password?
                auto username = credentials.substr(0, semicolon_pos);
                auto password = credentials.substr(semicolon_pos + 1);

                if (username.length() >= SCE_HTTP_USERNAME_MAX_SIZE)
                    return SCE_HTTP_ERROR_OUT_OF_SIZE;
                if (password.length() >= SCE_HTTP_USERNAME_MAX_SIZE)
                    return SCE_HTTP_ERROR_OUT_OF_SIZE;

                out.username = username;
                out.password = password;
            } else { // we don't have password
                out.username = credentials;
            }
        } else { // no credentials
            // lttstore.com:727
            auto semicolon_pos = full_wo_scheme.find(':');

            if (semicolon_pos != std::string::npos) { // we have port
                auto hostname = full_wo_scheme.substr(0, semicolon_pos);
                auto port = full_wo_scheme.substr(semicolon_pos + 1);

                out.hostname = hostname;
                out.port = port;
            } else { // no port
                out.hostname = full_wo_scheme;
            }
        }
    }

    return SceHttpErrorCode{};
}

// Indices must stay in sync with SceHttpMethods
constexpr std::array method_names{
    "GET"sv, "POST"sv, "HEAD"sv, "OPTIONS"sv, "PUT"sv, "DELETE"sv, "TRACE"sv, "CONNECT"sv
};

int char_method_to_int(const char *method) {
    if (!method)
        return -1;

    const auto it = std::ranges::find(method_names, std::string_view(method));
    if (it == method_names.end())
        return -1;

    return static_cast<int>(std::ranges::distance(method_names.begin(), it));
}

const char *int_method_to_char(const int n) {
    if (n < 0 || std::cmp_greater_equal(n, method_names.size()))
        return "INVALID";

    return method_names[static_cast<size_t>(n)].data();
}

std::string constructHeaders(const HeadersMapType &headers) {
    std::string headersString;
    for (const auto &[name, value] : headers)
        fmt::format_to(std::back_inserter(headersString), "{}: {}\r\n", name, value);

    return headersString;
}

bool parseStatusLine(std::string_view line, std::string &httpVer, int &statusCode, std::string &reason) {
    constexpr auto version_prefix = "HTTP/"sv;

    const auto lineClean = line.substr(0, line.find("\r\n"sv));

    // do this check just in case the server is drunk, would be nice to do more checks with some regex
    if (!lineClean.starts_with(version_prefix))
        return false; // what

    const auto firstSpace = lineClean.find(' ');
    if (firstSpace == std::string_view::npos)
        return false;

    const auto fullHttpVerStr = lineClean.substr(0, firstSpace);
    const auto httpVerStr = fullHttpVerStr.substr(version_prefix.length());

    if (httpVerStr.empty() || !is_digit(httpVerStr.front()))
        return false;

    if (lineClean.length() < fullHttpVerStr.length() + " XXX"sv.length())
        return false; // the rest of the line is less than 3 characters in length, abort

    const auto codeAndReason = lineClean.substr(firstSpace + 1);
    const auto statusCodeStr = codeAndReason.substr(0, 3);
    if (!std::ranges::all_of(statusCodeStr, is_digit))
        return false; // status code contains non digit characters, abort

    int statusCodeInt = 0;
    std::from_chars(statusCodeStr.data(), statusCodeStr.data() + statusCodeStr.size(), statusCodeInt);

    httpVer = httpVerStr;
    statusCode = statusCodeInt;
    // standard says that reasons CAN be empty, we have to take this edge case into account
    reason = codeAndReason.contains(' ') ? std::string(codeAndReason.substr(4)) : std::string();

    return true;
}

/*
    CANNOT have ANYTHING after the last \r\n or \r\n\r\n else it will be treated as a header
*/
bool parseHeaders(std::string_view headersRaw, HeadersMapType &headersOut) {
    for (const auto raw_line : std::views::split(headersRaw, '\n')) {
        std::string_view line(raw_line);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.empty())
            continue;

        const auto separator = line.find(':');
        if (separator == std::string_view::npos)
            return false; // separator is missing, the header is invalid

        const auto name = line.substr(0, separator);
        auto value = line.substr(separator + 1);
        if (value.starts_with(' '))
            value.remove_prefix(1);

        headersOut.emplace(name, value);
    }
    return true;
}

bool parseResponse(const std::string &res, SceRequestResponse &reqres) {
    const std::string_view response(res);
    const auto statusLineEnd = response.find("\r\n"sv);
    if (!parseStatusLine(response.substr(0, statusLineEnd), reqres.httpVer, reqres.statusCode, reqres.reasonPhrase))
        return false;

    const auto headersStart = statusLineEnd == std::string_view::npos ? response.size() : statusLineEnd + 2;
    if (!parseHeaders(response.substr(headersStart), reqres.headers))
        return false;

    const auto contLenIt = reqres.headers.find("Content-Length");
    reqres.contentLength = contLenIt == reqres.headers.end() ? 0 : std::stoi(contLenIt->second);

    return true;
}

bool socketSetBlocking(int sockfd, bool blocking) {
#ifdef _WIN32
    u_long blocking_tmp = blocking;
    ioctlsocket(sockfd, FIONBIO, &blocking_tmp);
#else
    if (blocking) { // Blocking
        int flags = fcntl(sockfd, F_GETFL); // Get flags
        fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK); // Set NONBLOCK flag off
    } else { // Non blocking
        int flags = fcntl(sockfd, F_GETFL); // Get flags
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK); // Set NONBLOCK flag on
    }
#endif
    return true;
}

std::string get_web_response(const std::string &url) {
    auto curl = curl_easy_init();
    if (!curl)
        return {};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Vita3K Emulator");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true); // Follow redirects
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

#ifdef __ANDROID__
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
#endif

    std::string response_string;
    const auto writeFunc = +[](void *ptr, size_t size, size_t nmemb, std::string *data) {
        data->append(static_cast<const char *>(ptr), size * nmemb);
        return size * nmemb;
    };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    long response_code;
    curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_easy_cleanup(curl);

    if (response_code / 100 == 2)
        return response_string;

    return {};
}

std::string get_web_regex_result(const std::string &url, const std::regex &regex) {
    std::string result;

    // Get the response of the web
    const auto response = get_web_response(url);

    // Check if the response is not empty
    if (!response.empty()) {
        std::smatch match;
        // Check if the response matches the regex
        if (std::regex_search(response, match, regex)) {
            result = match[1];
        } else
            LOG_ERROR("No success found regex: {}", response);
    }

    return result;
}

std::vector<AssignedAddr> get_all_assigned_addrs() {
    std::vector<AssignedAddr> out_addrs;
    const auto ret_addrs = [&out_addrs]() {
        if (out_addrs.empty())
            out_addrs.push_back({ "localhost", "127.0.0.1", "255.255.255.255" });

        return out_addrs;
    };

#ifdef _WIN32
    std::vector<std::byte> buffer(sizeof(IP_ADAPTER_INFO));
    auto adapter_info = [&] { return reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data()); };

    // Make an initial call to GetAdaptersInfo to get the necessary size into out_buf_len
    ULONG out_buf_len = static_cast<ULONG>(buffer.size());
    if (GetAdaptersInfo(adapter_info(), &out_buf_len) == ERROR_BUFFER_OVERFLOW)
        buffer.resize(out_buf_len);

    const DWORD ret_val = GetAdaptersInfo(adapter_info(), &out_buf_len);
    if (ret_val != NO_ERROR) {
        LOG_CRITICAL("GetAdaptersInfo failed with error: {}", ret_val);
        return ret_addrs();
    }

    for (auto adapter = adapter_info(); adapter; adapter = adapter->Next) {
        for (auto ip_addr = &adapter->IpAddressList; ip_addr; ip_addr = ip_addr->Next) {
            if (std::string_view(ip_addr->IpAddress.String) != "0.0.0.0"sv)
                out_addrs.push_back({ adapter->Description, ip_addr->IpAddress.String, ip_addr->IpMask.String });
        }
    }
#else
    ifaddrs *if_addrs = nullptr;
    if (getifaddrs(&if_addrs) != 0)
        return ret_addrs();

    const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> if_addrs_owner(if_addrs, &freeifaddrs);

    for (const ifaddrs *ifa = if_addrs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_netmask || (ifa->ifa_flags & IFF_LOOPBACK) != 0)
            continue;
        if (ifa->ifa_addr->sa_family != AF_INET) // check it is IP4
            continue;

        const auto netMaskAddr = reinterpret_cast<const sockaddr_in *>(ifa->ifa_netmask)->sin_addr;
        char netMaskAddrStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &netMaskAddr, netMaskAddrStr, INET_ADDRSTRLEN);

        const auto hostAddr = reinterpret_cast<const sockaddr_in *>(ifa->ifa_addr)->sin_addr;
        char addressBuffer[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &hostAddr, addressBuffer, INET_ADDRSTRLEN);

        out_addrs.push_back({ ifa->ifa_name, addressBuffer, netMaskAddrStr });
    }
#endif
    return ret_addrs();
}

AssignedAddr get_selected_assigned_addr(int32_t &outIndex) {
    const auto addrs = get_all_assigned_addrs();
    if (outIndex < 0 || std::cmp_greater_equal(outIndex, addrs.size())) {
        LOG_ERROR("Invalid index {}, returning first address", outIndex);
        outIndex = 0;
    }
    return addrs[outIndex];
}

void init_address(int32_t &outIndex, uint32_t &netAddr, uint32_t &broadcastAddr) {
    // Initialize the net and broadcast address based on the assigned address and netmask
    const auto addr = get_selected_assigned_addr(outIndex);
    uint32_t netMask = 0;
    inet_pton(AF_INET, addr.addr.c_str(), &netAddr);
    inet_pton(AF_INET, addr.netMask.c_str(), &netMask);
    broadcastAddr = netAddr | ~netMask;
}

} // namespace net_utils
