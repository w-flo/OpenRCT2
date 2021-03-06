/*****************************************************************************
 * Copyright (c) 2014-2019 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "Crash.h"

#ifdef USE_BREAKPAD
#    include <map>
#    include <memory>
#    include <stdio.h>

#    if defined(_WIN32)
#        include <ShlObj.h>
#        include <client/windows/handler/exception_handler.h>
#        include <common/windows/http_upload.h>
#        include <string>
#    else
#        error Breakpad support not implemented yet for this platform
#    endif

#    include "../Version.h"
#    include "../config/Config.h"
#    include "../core/Console.hpp"
#    include "../core/String.hpp"
#    include "../interface/Screenshot.h"
#    include "../localisation/Language.h"
#    include "../rct2/S6Exporter.h"
#    include "../scenario/Scenario.h"
#    include "../util/Util.h"
#    include "platform.h"

#    define WSZ(x) L"" x

#    ifdef OPENRCT2_COMMIT_SHA1_SHORT
const wchar_t* _wszCommitSha1Short = WSZ(OPENRCT2_COMMIT_SHA1_SHORT);
#    else
const wchar_t* _wszCommitSha1Short = WSZ("");
#    endif

// OPENRCT2_ARCHITECTURE is required to be defined in version.h
const wchar_t* _wszArchitecture = WSZ(OPENRCT2_ARCHITECTURE);

// Note: uploading gzipped crash dumps manually requires specifying
// 'Content-Encoding: gzip' header in HTTP request, but we cannot do that,
// so just hope the file name with '.gz' suffix is enough.
// For docs on uplading to backtrace.io check
// https://documentation.backtrace.io/product_integration_minidump_breakpad/
static bool UploadMinidump(const std::map<std::wstring, std::wstring>& files, int& error, std::wstring& response)
{
    for (auto file : files)
    {
        wprintf(L"files[%s] = %s\n", file.first.c_str(), file.second.c_str());
    }
    std::wstring url(L"https://openrct2.sp.backtrace.io:6098/"
                     L"post?format=minidump&token=27bfc474b8739e7c1df37180727e717a0a95d3bf3f2a8eaaf17ad321fb179c6f");
    std::map<std::wstring, std::wstring> parameters;
    parameters[L"product_name"] = L"openrct2";
    // In case of releases this can be empty
    if (wcslen(_wszCommitSha1Short) > 0)
    {
        parameters[L"commit"] = _wszCommitSha1Short;
    }
    else
    {
        parameters[L"commit"] = String::ToUtf16(gVersionInfoFull);
    }
    int timeout = 10000;
    bool success = google_breakpad::HTTPUpload::SendRequest(url, parameters, files, &timeout, &response, &error);
    wprintf(L"Success = %d, error = %d, response = %s\n", success, error, response.c_str());
    return success;
}

static bool OnCrash(
    const wchar_t* dumpPath, const wchar_t* miniDumpId, void* context, EXCEPTION_POINTERS* exinfo,
    MDRawAssertionInfo* assertion, bool succeeded)
{
    if (!succeeded)
    {
        constexpr const char* DumpFailedMessage = "Failed to create the dump. Please file an issue with OpenRCT2 on GitHub and "
                                                  "provide latest save, and provide "
                                                  "information about what you did before the crash occurred.";
        printf("%s\n", DumpFailedMessage);
        if (!gOpenRCT2SilentBreakpad)
        {
            MessageBoxA(nullptr, DumpFailedMessage, OPENRCT2_NAME, MB_OK | MB_ICONERROR);
        }
        return succeeded;
    }

    std::map<std::wstring, std::wstring> uploadFiles;

    // Get filenames
    wchar_t dumpFilePath[MAX_PATH];
    wchar_t saveFilePath[MAX_PATH];
    wchar_t configFilePath[MAX_PATH];
    wchar_t saveFilePathGZIP[MAX_PATH];
    swprintf_s(dumpFilePath, sizeof(dumpFilePath), L"%s\\%s.dmp", dumpPath, miniDumpId);
    swprintf_s(saveFilePath, sizeof(saveFilePath), L"%s\\%s.sv6", dumpPath, miniDumpId);
    swprintf_s(configFilePath, sizeof(configFilePath), L"%s\\%s.ini", dumpPath, miniDumpId);
    swprintf_s(saveFilePathGZIP, sizeof(saveFilePathGZIP), L"%s\\%s.sv6.gz", dumpPath, miniDumpId);

    wchar_t dumpFilePathNew[MAX_PATH];
    swprintf_s(
        dumpFilePathNew, sizeof(dumpFilePathNew), L"%s\\%s(%s_%s).dmp", dumpPath, miniDumpId, _wszCommitSha1Short,
        _wszArchitecture);

    wchar_t dumpFilePathGZIP[MAX_PATH];
    swprintf_s(dumpFilePathGZIP, sizeof(dumpFilePathGZIP), L"%s.gz", dumpFilePathNew);

    // Compress the dump
    {
        FILE* input = _wfopen(dumpFilePath, L"rb");
        FILE* dest = _wfopen(dumpFilePathGZIP, L"wb");

        if (util_gzip_compress(input, dest))
        {
            // TODO: enable upload of gzip-compressed dumps once supported on
            // backtrace.io (uncomment the line below). For now leave compression
            // on, as GitHub will accept .gz files, even though it does not
            // advertise it officially.

            /*
            uploadFiles[L"upload_file_minidump"] = dumpFilePathGZIP;
            */
        }
        fclose(input);
        fclose(dest);
    }

    // Try to rename the files
    if (_wrename(dumpFilePath, dumpFilePathNew) == 0)
    {
        std::wcscpy(dumpFilePath, dumpFilePathNew);
    }
    uploadFiles[L"upload_file_minidump"] = dumpFilePath;

    // Compress to gzip-compatible stream

    // Log information to output
    wprintf(L"Dump Path: %s\n", dumpPath);
    wprintf(L"Dump File Path: %s\n", dumpFilePath);
    wprintf(L"Dump Id: %s\n", miniDumpId);
    wprintf(L"Version: %s\n", WSZ(OPENRCT2_VERSION));
    wprintf(L"Commit: %s\n", _wszCommitSha1Short);

    bool savedGameDumped = false;
    utf8* saveFilePathUTF8 = widechar_to_utf8(saveFilePath);
    try
    {
        auto exporter = std::make_unique<S6Exporter>();
        exporter->Export();
        exporter->SaveGame(saveFilePathUTF8);
        savedGameDumped = true;
    }
    catch (const std::exception&)
    {
    }
    free(saveFilePathUTF8);

    // Compress the save
    if (savedGameDumped)
    {
        FILE* input = _wfopen(saveFilePath, L"rb");
        FILE* dest = _wfopen(saveFilePathGZIP, L"wb");

        if (util_gzip_compress(input, dest))
        {
            uploadFiles[L"attachment_park.sv6.gz"] = saveFilePathGZIP;
        }
        else
        {
            uploadFiles[L"attachment_park.sv6"] = saveFilePath;
        }
        fclose(input);
        fclose(dest);
    }

    utf8* configFilePathUTF8 = widechar_to_utf8(configFilePath);
    if (config_save(configFilePathUTF8))
    {
        uploadFiles[L"attachment_config.ini"] = configFilePath;
    }
    free(configFilePathUTF8);

    std::string screenshotPath = screenshot_dump();
    if (!screenshotPath.empty())
    {
        wchar_t* screenshotPathWchar = utf8_to_widechar(screenshotPath.c_str());
        auto screenshotPathW = std::wstring(screenshotPathWchar);
        free(screenshotPathWchar);
        uploadFiles[L"attachment_screenshot.png"] = screenshotPathW;
    }

    if (gOpenRCT2SilentBreakpad)
    {
        int error;
        std::wstring response;
        UploadMinidump(uploadFiles, error, response);
        return succeeded;
    }

    constexpr const wchar_t* MessageFormat = L"A crash has occurred and a dump was created at\n%s.\n\nPlease file an issue "
                                             L"with OpenRCT2 on GitHub, and provide "
                                             L"the dump and saved game there.\n\nVersion: %s\nCommit: %s\n\n"
                                             L"We would like to upload the crash dump for automated analysis, do you agree?\n"
                                             L"The automated analysis is done by courtesy of https://backtrace.io/";
    wchar_t message[MAX_PATH * 2];
    swprintf_s(message, MessageFormat, dumpFilePath, WSZ(OPENRCT2_VERSION), _wszCommitSha1Short);

    // Cannot use platform_show_messagebox here, it tries to set parent window already dead.
    int answer = MessageBoxW(nullptr, message, WSZ(OPENRCT2_NAME), MB_YESNO | MB_ICONERROR);
    if (answer == IDYES)
    {
        int error;
        std::wstring response;
        bool ok = UploadMinidump(uploadFiles, error, response);
        if (!ok)
        {
            const wchar_t* MessageFormat2 = L"There was a problem while uploading the dump. Please upload it manually to "
                                            L"GitHub. It should be highlighted for you once you close this message.\n"
                                            L"Please provide following information as well:\n"
                                            L"Error code = %d\n"
                                            L"Response = %s";
            swprintf_s(message, MessageFormat2, error, response.c_str());
            MessageBoxW(nullptr, message, WSZ(OPENRCT2_NAME), MB_OK | MB_ICONERROR);
        }
        else
        {
            MessageBoxW(nullptr, L"Dump uploaded succesfully.", WSZ(OPENRCT2_NAME), MB_OK | MB_ICONINFORMATION);
        }
    }
    HRESULT coInitializeResult = CoInitialize(nullptr);
    if (SUCCEEDED(coInitializeResult))
    {
        LPITEMIDLIST pidl = ILCreateFromPathW(dumpPath);
        LPITEMIDLIST files[3];
        uint32_t numFiles = 0;

        files[numFiles++] = ILCreateFromPathW(dumpFilePath);
        // There should be no need to check if this file exists, if it doesn't
        // it simply shouldn't get selected.
        files[numFiles++] = ILCreateFromPathW(dumpFilePathGZIP);
        if (savedGameDumped)
        {
            files[numFiles++] = ILCreateFromPathW(saveFilePath);
        }
        if (pidl != nullptr)
        {
            SHOpenFolderAndSelectItems(pidl, numFiles, (LPCITEMIDLIST*)files, 0);
            ILFree(pidl);
            for (uint32_t i = 0; i < numFiles; i++)
            {
                ILFree(files[i]);
            }
        }
        CoUninitialize();
    }

    // Return whether the dump was successful
    return succeeded;
}

static std::wstring GetDumpDirectory()
{
    char userDirectory[MAX_PATH];
    platform_get_user_directory(userDirectory, nullptr, sizeof(userDirectory));

    wchar_t* userDirectoryW = utf8_to_widechar(userDirectory);
    auto result = std::wstring(userDirectoryW);
    free(userDirectoryW);

    return result;
}

// Using non-null pipe name here lets breakpad try setting OOP crash handling
constexpr const wchar_t* PipeName = L"openrct2-bpad";

#endif // USE_BREAKPAD

CExceptionHandler crash_init()
{
#ifdef USE_BREAKPAD
    // Path must exist and be RW!
    auto exHandler = new google_breakpad::ExceptionHandler(
        GetDumpDirectory(), 0, OnCrash, 0, google_breakpad::ExceptionHandler::HANDLER_ALL, MiniDumpWithDataSegs, PipeName, 0);
    return reinterpret_cast<CExceptionHandler>(exHandler);
#else  // USE_BREAKPAD
    return nullptr;
#endif // USE_BREAKPAD
}
