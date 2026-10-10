#pragma once
// Included in heap_corruption_capture_test's anonymous namespace to reuse
// its process, directory, marker and dump-reading helpers.
void CheckReportStreams(const std::wstring& path, DWORD exceptionCode) {
    ULONGLONG size = 0;
    CHECK(DumpedExceptionCode(path, size) == exceptionCode);
    CHECK(size > 0 && size <= ReportDumpLimit);
    CHECK(!FileContainsMarker(path));
    std::ifstream input(path.c_str(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(bytes.size() >= sizeof(MINIDUMP_HEADER));
    const auto* header = reinterpret_cast<const MINIDUMP_HEADER*>(bytes.data());
    // DbgHelp may add register state such as MiniDumpWithAvxXStateContext.
    CHECK((header->Flags & ReportDumpType) == ReportDumpType);
    CHECK(!(header->Flags & (MiniDumpWithPrivateReadWriteMemory | MiniDumpWithFullMemory | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs)));
    CHECK(!(header->Flags & (MiniDumpWithHandleData | MiniDumpWithFullMemoryInfo | MiniDumpWithProcessThreadData |
        MiniDumpWithPrivateWriteCopyMemory | MiniDumpWithCodeSegs | MiniDumpWithTokenInformation) & ~ReportDumpType));
    for (const ULONG type : { ULONG(ExceptionStream), ULONG(ThreadListStream), ULONG(ModuleListStream), ULONG(ThreadInfoListStream) }) {
        MINIDUMP_DIRECTORY* directory = nullptr; void* stream = nullptr; ULONG length = 0;
        CHECK(MiniDumpReadDumpStream(bytes.data(), type, &directory, &stream, &length));
        CHECK(stream && length);
    }
}
enum class FullFailure { Denied, Timeout };
FullFailure fullFailure;
std::wstring reportBeforeFull;
int reportWrites = 0, fullWrites = 0;
BOOL WINAPI FailingFullWriter(HANDLE process, DWORD processId, HANDLE file, MINIDUMP_TYPE type,
    PMINIDUMP_EXCEPTION_INFORMATION exception, PMINIDUMP_USER_STREAM_INFORMATION streams, PMINIDUMP_CALLBACK_INFORMATION callback) {
    CHECK(exception && !exception->ClientPointers);
    if (type == ReportDumpType) {
        ++reportWrites;
        return MiniDumpWriteDump(process, processId, file, type, exception, streams, callback);
    }
    ++fullWrites;
    // The small dump is already published, before even the first full attempt.
    const auto paths = DumpFiles(reportBeforeFull.c_str(), L"*-report.dmp");
    CHECK(paths.size() == 1);
    CheckReportStreams(reportBeforeFull + L"\\" + paths.front(), HeapCorruptionCode);
    if (fullFailure == FullFailure::Timeout) {
        CHECK(callback);
        MINIDUMP_CALLBACK_INPUT input{}; input.CallbackType = CancelCallback;
        MINIDUMP_CALLBACK_OUTPUT output{};
        do {
            Sleep(1);
            CHECK(callback->CallbackRoutine(callback->CallbackParam, &input, &output));
            CHECK(output.CheckCancel);
        } while (!output.Cancel);
        SetLastError(ERROR_CANCELLED);
    } else SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}
BOOL WINAPI OversizedReportWriter(HANDLE, DWORD, HANDLE file, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION) {
    LARGE_INTEGER size{}; size.QuadPart = ReportDumpLimit + 1;
    return SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file);
}
void TestSmallDumpIndependentOfFull() {
    LeaveUnreferencedMarker();
    for (const auto failure : { FullFailure::Denied, FullFailure::Timeout }) {
        const auto logs = TempDirectory();
        DumpChannel channel; CHECK(channel.Create()); channel.reportEnabled = true;
        CONTEXT context{}; RtlCaptureContext(&context);
        EXCEPTION_RECORD record{}; record.ExceptionCode = HeapCorruptionCode;
        record.ExceptionAddress = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&TestSmallDumpIndependentOfFull));
        EXCEPTION_POINTERS pointers{ &record, &context };
        channel.view->magic = DumpRequestMagic; channel.view->threadId = GetCurrentThreadId();
        channel.view->exceptionPointers = reinterpret_cast<uintptr_t>(&pointers);
        fullFailure = failure; reportBeforeFull = logs; reportWrites = fullWrites = 0;
        CHECK(!channel.Serve(GetCurrentProcess(), logs.c_str(), FailingFullWriter, 2000));
        CHECK(reportWrites == 1 && fullWrites >= 1);
        CHECK(!channel.written[0] && !channel.reportWritten.empty() && channel.view->written == 1);
        CHECK(WaitForSingleObject(channel.done, 0) == WAIT_OBJECT_0);
        CheckReportStreams(channel.reportWritten, HeapCorruptionCode);
        CHECK(DumpFiles(logs.c_str(), L"*.partial").empty());
        PruneDumps(logs.c_str(), 0, channel.reportWritten.c_str());
        CHECK(GetFileAttributesW(channel.reportWritten.c_str()) != INVALID_FILE_ATTRIBUTES);
        PruneDumps(logs.c_str(), 0); CHECK(CountDumps(logs) == 0);
        channel.Close(); RemoveDirectoryW(logs.c_str());
    }
    // Oversized successful writes are never published, and leave no partial.
    const auto logs = TempDirectory();
    const auto path = logs + L"\\sf4e-crash-size-report.dmp";
    const MINIDUMP_TYPE type = ReportDumpType;
    CHECK(!PublishDump(CreateDumpFile(path.c_str()), path.c_str(), GetCurrentProcess(), GetCurrentProcessId(),
        GetCurrentThreadId(), nullptr, false, { &type, 1, ReportDumpLimit, 0, OversizedReportWriter }));
    CHECK(FileAbsent(path.c_str())); CHECK(DumpFiles(logs.c_str(), L"*").empty());
    RemoveDirectoryW(logs.c_str());
}
