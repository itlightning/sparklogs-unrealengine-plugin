// Copyright (C) 2024-2025 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

#include "Misc/AutomationTest.h"
#include "CoreTypes.h"
#include "Containers/UnrealString.h"
#include "GenericPlatform/GenericPlatform.h"
#include "Templates/UniquePtr.h"
#include "Templates/SharedPointer.h"
#include "Algo/Compare.h"
#include "sparklogs.h"

template<typename T>
class FScopedValueSetter
{
public:
    FScopedValueSetter(T& Target, T TemporaryValue)
        : Reference(Target)
        , OriginalValue(Target)
    {
        Reference = TemporaryValue;
    }

    ~FScopedValueSetter()
    {
        Reference = OriginalValue;
    }

private:
    T& Reference;
    T OriginalValue;
};

class FITLTestTempDirectory
{
public:
    FITLTestTempDirectory(const FString& InDirectoryPath, int InInstanceIndex) : DirectoryPath(InDirectoryPath), InstanceIndex(InInstanceIndex)
    {
        Created = false;
        if (!IFileManager::Get().DirectoryExists(*DirectoryPath))
        {
            Created = IFileManager::Get().MakeDirectory(*DirectoryPath);
        }
        ClearUserProgressMarker();
    }
    ~FITLTestTempDirectory()
    {
        if (Created)
        {
            IFileManager::Get().DeleteDirectory(*DirectoryPath, false, true);
        }
        ClearUserProgressMarker();
    }
    FString GetTempDir() { return DirectoryPath; }
    void ClearUserProgressMarker()
    {
        // Manually clear out the temporary state file for this test run
        FString StateIni = ITLGetIndexedStateFileINI(InstanceIndex);
        if (StateIni != GGameUserSettingsIni)
        {
            // We've got to purge the information from the UE INI cache as well or it will still be readable in future tests!!!
            GConfig->EmptySection(ITL_CONFIG_SECTION_NAME, StateIni);
            GConfig->Flush(false, StateIni);
            GConfig->Remove(StateIni);
            IFileManager::Get().Delete(*StateIni, false, false, true);
        }
    }

private:
    bool Created;
    FString DirectoryPath;
    int InstanceIndex;
};

static FString ITLGetTestDir()
{
    FString ParentDir = FPaths::ProjectIntermediateDir();
    if (ParentDir.Len() <= 0) { ParentDir = FPaths::ProjectDir(); }
    if (ParentDir.Len() <= 0) { ParentDir = FPaths::ProjectSavedDir(); }
    check(ParentDir.Len() > 0);
    FString TempDir = FPaths::CreateTempFilename(*ParentDir, TEXT("itl-test-"), TEXT(""));
    TempDir.AppendInt(FMath::Rand());
    TempDir.Append(TEXT(".tmp"));
    check(TempDir.Len() > 0);
    return TempDir;
}

static void ITLWriteStringToFile(TSharedRef<IFileHandle> FileHandle, const TCHAR* Str)
{
    FTCHARToUTF8 Converter(Str);
    FileHandle->Write((const uint8*)(Converter.Get()), Converter.Length());
}

/** A payload processor that keeps the payload in memory. */
class FsparklogsStoreInMemPayloadProcessor : public IsparklogsPayloadProcessor
{
public:
    bool FailProcessing;
    TArray<FString> Payloads;
    int LastOriginalPayloadLen;
    FsparklogsStoreInMemPayloadProcessor() : FailProcessing(false) { }
    virtual bool ProcessPayload(TArray<uint8>& JSONPayloadInUTF8, int PayloadLen, int OriginalPayloadLen, ITLCompressionMode CompressionMode, TWeakPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> StreamerWeakPtr) override
    {
        LastOriginalPayloadLen = OriginalPayloadLen;
        if (FailProcessing)
        {
            ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("TEST: forcefully failing processing of payload of length %d"), PayloadLen);
            return false;
        }
        TArray<uint8> DecompressedData;
        if (!ITLDecompressData(CompressionMode, JSONPayloadInUTF8.GetData(), PayloadLen, OriginalPayloadLen, DecompressedData))
        {
            UE_LOG(LogPluginSparkLogs, Warning, TEXT("TEST: failed to decompress data in payload: mode=%d, len=%d, original_len=%d"), (int)CompressionMode, PayloadLen, OriginalPayloadLen);
            return false;
        }
        Payloads.Add(ITLConvertUTF8(DecompressedData.GetData(), DecompressedData.Num()));
        return true;
    }
};

bool ITLComparePayloads(FAutomationTestBase* T, const TArray<FString>& Actual, const TArray<FString>& Expected)
{
    if (!Algo::Compare(Actual, Expected))
    {
        TStringBuilder<4096> b;
        b.Append(TEXT("Expected:\t\n"));
        for (const FString& p : Expected) { b.Append(p); b.Append(TEXT("\r\n")); }
        b.Append(TEXT("Got:\t\n"));
        for (const FString& p : Actual) { b.Append(p); b.Append(TEXT("\r\n")); }
        b.Append(TEXT("(END)\t\n"));
        T->AddError(b.ToString());
        return false;
    }
    else
    {
        return true;
    }
}

static void SetupCompressionModes(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands)
{
    OutBeautifiedNames.Add(TEXT("uncompressed"));
    OutTestCommands.Add(FString::FromInt((int)ITLCompressionMode::None));
    OutBeautifiedNames.Add(TEXT("LZ4"));
    OutTestCommands.Add(FString::FromInt((int)ITLCompressionMode::LZ4));
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestSkipByteMarker, "sparklogs.UnitTests.SkipByteMarker", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestSkipByteMarker::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestSkipByteMarker::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));
    
    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    uint8 TestUTF8ByteOrderMark[3] = { 0xEF, 0xBB, 0xBF };
    LogWriter->Write(TestUTF8ByteOrderMark, 3);
    ITLWriteStringToFile(LogWriter, TEXT("Hello world!!"));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    bool FlushedEverything=false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Now that we have a newline, it should flush and capture everything
    ExpectedPayloads.Add(FString(TEXT("[{\"message\":\"Hello world!!\"}]")));
    ITLWriteStringToFile(LogWriter, TEXT("\r\n"));
    LogWriter->Flush();
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestSkipEmptyPayloads, "sparklogs.UnitTests.SkipEmptyPayloads", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestSkipEmptyPayloads::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestSkipEmptyPayloads::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    
    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    // Test completely empty file
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[1] should capture everything"), FlushedEverything);

    // Test one blank line
    ITLWriteStringToFile(LogWriter, TEXT("\n"));
    LogWriter->Flush();
    TestTrue(TEXT("FlushAndWait[2] should succeed"), Streamer->FlushAndWait(1, false, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[2] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[2] should capture everything"), FlushedEverything);

    // Test additional flushes without changes in log file
    TestTrue(TEXT("FlushAndWait[3] should succeed"), Streamer->FlushAndWait(5, false, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[3] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[3] should capture everything"), FlushedEverything);

    // Test several blank lines and a partial last line
    ITLWriteStringToFile(LogWriter, TEXT("\r\n\n\n\n\r\n\r\n    "));
    LogWriter->Flush();
    TestTrue(TEXT("FlushAndWait[4] should succeed"), Streamer->FlushAndWait(2, false, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[4] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[4] should NOT capture everything"), FlushedEverything);

    // Test flush after no change in partial last line
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[FINAL] should NOT capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestMultipleEvents, "sparklogs.UnitTests.MultipleEvents", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestMultipleEvents::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestMultipleEvents::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("Line 1\r\nSecond line is longer\r\n3\r\n   fourth line    \t\r\n"));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"Line 1\"},{\"message\":\"Second line is longer\"},{\"message\":\"3\"},{\"message\":\"   fourth line    \\t\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestNewlines, "sparklogs.UnitTests.Newlines", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestNewlines::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestNewlines::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("\t\n\n\r\n\nlinux\nskip\rslash\rR\n \r\n \n"));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"\\t\"},{\"message\":\"linux\"},{\"message\":\"skip\\rslash\\rR\"},{\"message\":\" \"},{\"message\":\" \"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestControlChars, "sparklogs.UnitTests.ControlChars", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestControlChars::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestControlChars::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("line 1\t\b\f\r\nline 2 \"hello\"\r\nline 3 \\world\\\r\n"));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"line 1\\t\\b\\f\"},{\"message\":\"line 2 \\\"hello\\\"\"},{\"message\":\"line 3 \\\\world\\\\\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestUnicode, "sparklogs.UnitTests.Unicode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestUnicode::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestUnicode::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    const TCHAR* TestPayload1 = TEXT("Hello world in 2 languages: こんにちは世界   你好，世界");
    ITLWriteStringToFile(LogWriter, *FString::Format(TEXT("{0}\r\n"), { TestPayload1 }));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(FString::Format(TEXT("[{\"message\":\"{0}\"}]"), { TestPayload1 }));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestMaxLineSize, "sparklogs.UnitTests.MaxLineSize", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestMaxLineSize::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestMaxLineSize::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;
    constexpr int MaxLineSize = 8;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    // One line that is 2x size of max line, then an unfinished line that is 1/2x size of max line
    ITLWriteStringToFile(LogWriter, TEXT("1234567812345678\r\n1234"));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, MaxLineSize, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"12345678\"},{\"message\":\"12345678\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Finish the line that is 1/2x the max line size, then 1x that is exactly the same, then another line that is a little over, then end with an incomplete line
    ITLWriteStringToFile(LogWriter, TEXT("\r\n12345678\r\n1234567812\r\n123"));
    LogWriter->Flush();
    ExpectedPayloads.Add(TEXT("[{\"message\":\"1234\"},{\"message\":\"12345678\"},{\"message\":\"12345678\"},{\"message\":\"12\"}]"));
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[FINAL] should NOT capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestMaxLineSizeUnicode, "sparklogs.UnitTests.MaxLineSizeUnicode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestMaxLineSizeUnicode::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestMaxLineSizeUnicode::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;
    // IMPORTANT: this is in *bytes* and we do not split a line in the middle of a Unicode character
    constexpr int MaxLineSize = 8;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    // One line that is 2x size of max line, then an unfinished line that is 1/2x size of max line
    // NOTE: π requires 2 bytes to encode in UTF-8
    ITLWriteStringToFile(LogWriter, TEXT("1234ππ5678π34\r\n1π4"));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, MaxLineSize, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"1234\"},{\"message\":\"ππ5678\"},{\"message\":\"π34\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Finish the line that is 1/2x the max line size, then 1x that is exactly the same, then another line that is a little over, then end with an incomplete line
    // NOTE: Ω takes 3 bytes to encode in UTF-8
    ITLWriteStringToFile(LogWriter, TEXT("\r\n123Ω78\r\n12345Ωπ\r\nΩ\r\n"));
    LogWriter->Flush();
    ExpectedPayloads.Add(TEXT("[{\"message\":\"1π4\"},{\"message\":\"123Ω78\"},{\"message\":\"12345\"},{\"message\":\"Ωπ\"},{\"message\":\"Ω\"}]"));
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestStopAndResume, "sparklogs.UnitTests.StopAndResume", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestStopAndResume::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestStopAndResume::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("Line 1\r\nLine 2\r\n1234"));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"Line 1\"},{\"message\":\"Line 2\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1-FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1-FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1-FINAL] should NOT capture everything"), FlushedEverything);

    int64 ProgressMarker = 0;
    Streamer->ReadProgressMarker(ProgressMarker);
    TestEqual(TEXT("FlushAndWait[1-FINAL] progress marker should match"), ProgressMarker, (int64)16);
    Streamer.Reset();
    
    // When we resume, it should remember that we already processed the first two lines and not generate a new payload
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor2(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer2 = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor2, 16 * 1024, FString(), FString(), nullptr));
    Streamer2->SetWeakThisPtr(Streamer2);
    TArray<FString> ExpectedPayloads2;
    TestTrue(TEXT("FlushAndWait[2-1] should succeed"), Streamer2->FlushAndWait(2, false, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[2-1] payloads should match"), ITLComparePayloads(this, PayloadProcessor2->Payloads, ExpectedPayloads2));
    TestFalse(TEXT("FlushAndWait[2-1] should NOT capture everything"), FlushedEverything);

    // finish the partial line and make sure it gets captured and nothing else
    ITLWriteStringToFile(LogWriter, TEXT("Line 3\r\nLine 4\r\nlast line\r\n"));
    LogWriter->Flush();
    ExpectedPayloads2.Add(TEXT("[{\"message\":\"1234Line 3\"},{\"message\":\"Line 4\"},{\"message\":\"last line\"}]"));
    TestTrue(TEXT("FlushAndWait[2-FINAL] should succeed"), Streamer2->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[2-FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor2->Payloads, ExpectedPayloads2));
    TestTrue(TEXT("FlushAndWait[2-FINAL] should capture everything"), FlushedEverything);
    
    Streamer2.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestHandleLogRotation, "sparklogs.UnitTests.HandleLogRotation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestHandleLogRotation::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestHandleLogRotation::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("123456789012345678901234567890\r\n"));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"123456789012345678901234567890\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(2, false, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[1] should capture everything"), FlushedEverything);
    int64 ProgressMarker = 0;
    Streamer->ReadProgressMarker(ProgressMarker);
    TestEqual(TEXT("FlushAndWait[1] progress marker should match"), ProgressMarker, (int64)32);

    // Simulate the log file being rotated and add a new log line to it
    LogWriter->Seek(0);
    LogWriter->Truncate(0);
    LogWriter->Flush();
    ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("Truncated logfile size to %ld. logfile='%s'"), IFileManager::Get().FileSize(*TestLogFile), *TestLogFile);
    TestEqual(TEXT("Logfile should now have 0 size"), IFileManager::Get().FileSize(*TestLogFile), (int64)0);
    ITLWriteStringToFile(LogWriter, TEXT("Line 2\r\n"));
    LogWriter->Flush();

    ExpectedPayloads.Add(TEXT("[{\"message\":\"Line 2\"}]"));
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(3, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);
    Streamer->ReadProgressMarker(ProgressMarker);
    TestEqual(TEXT("FlushAndWait[FINAL] progress marker should match"), ProgressMarker, (int64)8);
    
    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestRetryDelay, "sparklogs.UnitTests.RetryDelay", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestRetryDelay::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestRetryDelay::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("Line 1\r\nLine 2\r\n1234"));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    // Setup so that we process success requests very quickly, but delay a long time after a failure
    constexpr double TestProcessingIntervalSecs = 0.1;
    constexpr double TestRetryIntervalSecs = 3.0;
    Settings->ProcessingIntervalSecs = TestProcessingIntervalSecs;
    Settings->RetryIntervalSecs = TestRetryIntervalSecs;
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"Line 1\"},{\"message\":\"Line 2\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, false, TestProcessingIntervalSecs * 5, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Setup more data to log, but simulate failure of the payload processor
    PayloadProcessor->FailProcessing = true;
    ITLWriteStringToFile(LogWriter, TEXT("Line 3\r\nLine 4"));
    LogWriter->Flush();
    TestFalse(TEXT("FlushAndWait[2] should fail because of failure to process"), Streamer->FlushAndWait(1, false, false, false, TestProcessingIntervalSecs * 5, FlushedEverything));
    TestFalse(TEXT("FlushAndWait[2] should NOT capture everything"), FlushedEverything);

    // Make sure all manual flush requests have been processed
    FPlatformProcess::SleepNoStats(TestProcessingIntervalSecs * 5);

    // Even though processing the payload will no longer fail, we have to wait longer before a retry is allowed
    PayloadProcessor->FailProcessing = false;
    TestFalse(TEXT("FlushAndWait[3] should fail because of timeout waiting for processing to happen again"), Streamer->FlushAndWait(1, false, false, false, TestProcessingIntervalSecs * 5, FlushedEverything));
    TestFalse(TEXT("FlushAndWait[3] should NOT capture everything"), FlushedEverything);
    // Waiting longer than the retry interval should succeed
    ExpectedPayloads.Add(TEXT("[{\"message\":\"1234Line 3\"}]"));
    TestTrue(TEXT("FlushAndWait[4] should succeed because wait period is longer than retry interval"), Streamer->FlushAndWait(1, false, false, false, TestRetryIntervalSecs * 1.2, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[4] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[4] should NOT capture everything"), FlushedEverything);

    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[FINAL] should NOT capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestRetrySamePayloadSize, "sparklogs.UnitTests.RetrySamePayloadSize", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestRetrySamePayloadSize::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestRetrySamePayloadSize::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("Line 1\r\nLine 2\r\n1234"));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    // Setup so that we process success requests and retry requests very quickly.
    constexpr double TestProcessingIntervalSecs = 0.1;
    constexpr double TestRetryIntervalSecs = 0.1;
    Settings->ProcessingIntervalSecs = TestProcessingIntervalSecs;
    Settings->RetryIntervalSecs = TestRetryIntervalSecs;
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"Line 1\"},{\"message\":\"Line 2\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, false, TestProcessingIntervalSecs * 5, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Setup more data to log, but simulate failure of the payload processor
    PayloadProcessor->FailProcessing = true;
    ITLWriteStringToFile(LogWriter, TEXT("Line 3-ABCDEFG\r\nLine 4"));
    LogWriter->Flush();
    TestFalse(TEXT("FlushAndWait[2] should fail because of failure to process"), Streamer->FlushAndWait(1, false, false, false, TestProcessingIntervalSecs * 5, FlushedEverything));
    TestFalse(TEXT("FlushAndWait[2] should NOT capture everything"), FlushedEverything);
    TestNotEqual(TEXT("FlushAndWait[2] should have non-zero payload size"), PayloadProcessor->LastOriginalPayloadLen, 0);
    // Make sure this value is definitely set again the next time we process a payload
    int ExpectedOriginalPayloadLen = PayloadProcessor->LastOriginalPayloadLen;
    PayloadProcessor->LastOriginalPayloadLen = 0;

    // Write more data, and then make sure that a retried flush (that still fails)
    ITLWriteStringToFile(LogWriter, TEXT("\r\nLine 5\r\nLine 6 this is a long line!!!\r\n"));
    LogWriter->Flush();
    // Make sure all manual flush requests have been processed
    FPlatformProcess::SleepNoStats(TestRetryIntervalSecs * 5);
    TestFalse(TEXT("FlushAndWait[3] should fail because of failure to process"), Streamer->FlushAndWait(1, true, false, false, TestRetryIntervalSecs * 10, FlushedEverything));
    TestFalse(TEXT("FlushAndWait[3] should NOT capture everything"), FlushedEverything);
    TestEqual(TEXT("FlushAndWait[3] last payload read size same as before"), PayloadProcessor->LastOriginalPayloadLen, ExpectedOriginalPayloadLen);

    // One more iteration cycle to make sure that subsequent retries have the same behavior...
    PayloadProcessor->LastOriginalPayloadLen = 0;
    FPlatformProcess::SleepNoStats(TestRetryIntervalSecs * 10);
    TestFalse(TEXT("FlushAndWait[4] should fail because of failure to process"), Streamer->FlushAndWait(1, true, false, false, TestRetryIntervalSecs * 10, FlushedEverything));
    TestFalse(TEXT("FlushAndWait[4] should NOT capture everything"), FlushedEverything);
    TestEqual(TEXT("FlushAndWait[4] last payload read size same as before"), PayloadProcessor->LastOriginalPayloadLen, ExpectedOriginalPayloadLen);

    // Once we unblock the failure, make sure we fully capture everything after two cycles...
    PayloadProcessor->FailProcessing = false;
    ExpectedPayloads.Add(TEXT("[{\"message\":\"1234Line 3-ABCDEFG\"}]"));
    ExpectedPayloads.Add(TEXT("[{\"message\":\"Line 4\"},{\"message\":\"Line 5\"},{\"message\":\"Line 6 this is a long line!!!\"}]"));
    TestTrue(TEXT("FlushAndWait[5] should succeed"), Streamer->FlushAndWait(2, true, false, false, TestRetryIntervalSecs * 1.2, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[5] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[5] should capture everything"), FlushedEverything);
    TestTrue(TEXT("FlushAndWait[5] last payload should be larger"), PayloadProcessor->LastOriginalPayloadLen > ExpectedOriginalPayloadLen);

    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestClearRetryTimer, "sparklogs.UnitTests.ClearRetryTimer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestClearRetryTimer::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestClearRetryTimer::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("Line 1\r\nLine 2\r\n1234"));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    // Setup so that we process success requests very quickly, but delay a long time after a failure
    constexpr double TestProcessingIntervalSecs = 0.1;
    constexpr double TestRetryIntervalSecs = 3.0;
    Settings->ProcessingIntervalSecs = TestProcessingIntervalSecs;
    Settings->RetryIntervalSecs = TestRetryIntervalSecs;
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"Line 1\"},{\"message\":\"Line 2\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, false, TestProcessingIntervalSecs * 5, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Setup more data to log, but simulate failure of the payload processor
    PayloadProcessor->FailProcessing = true;
    ITLWriteStringToFile(LogWriter, TEXT("Line 3\r\nLine 4"));
    LogWriter->Flush();
    TestFalse(TEXT("FlushAndWait[2] should fail because of failure to process"), Streamer->FlushAndWait(1, false, false, false, TestProcessingIntervalSecs * 5, FlushedEverything));
    TestFalse(TEXT("FlushAndWait[2] should NOT capture everything"), FlushedEverything);

    // Make sure all manual flush requests have been processed
    FPlatformProcess::SleepNoStats(TestProcessingIntervalSecs * 5);

    // Even though processing the payload will no longer fail, we have to wait longer before a retry is allowed.
    // HOWEVER, clear the retry timer in this attempt, so it should succeed immediately!
    PayloadProcessor->FailProcessing = false;
    ExpectedPayloads.Add(TEXT("[{\"message\":\"1234Line 3\"}]"));
    TestTrue(TEXT("FlushAndWait[3] should succeed because the retry timer was cleared"), Streamer->FlushAndWait(1, true, false, false, TestProcessingIntervalSecs * 5, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[3] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[3] should NOT capture everything"), FlushedEverything);

    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[FINAL] should NOT capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestGameInstanceID, "sparklogs.UnitTests.GameInstanceID", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestGameInstanceID::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestGameInstanceID::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    const TCHAR* TestPayload1 = TEXT("Hello world");
    ITLWriteStringToFile(LogWriter, *FString::Format(TEXT("{0}\r\n"), { TestPayload1 }));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->AddRandomGameInstanceID = true;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), TEXT("abcd_1234_EFGH"), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(FString::Format(TEXT("[{\"game_instance_id\": \"abcd_1234_EFGH\", \"game_instance_index\": {0},\"message\":\"{1}\"}]"), { TestInstanceIndex, TestPayload1 }));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginUnitTestAdditionalAttributes, "sparklogs.UnitTests.AdditionalAttributes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginUnitTestAdditionalAttributes::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginUnitTestAdditionalAttributes::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    const TCHAR* TestPayload1 = TEXT("Hello world");
    ITLWriteStringToFile(LogWriter, *FString::Format(TEXT("{0}\r\n"), { TestPayload1 }));
    LogWriter->Flush();

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TMap<FString, FString> AdditionalAttributes;
    AdditionalAttributes.Add(TEXT("game_version"), TEXT("v1.2.3"));
    AdditionalAttributes.Add(TEXT("game_name"), TEXT("hello world"));
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), &AdditionalAttributes));
    Streamer->SetWeakThisPtr(Streamer);
    ExpectedPayloads.Add(FString::Format(TEXT("[{\"game_version\":\"v1.2.3\",\"game_name\":\"hello world\",\"message\":\"{0}\"}]"), { TestPayload1 }));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginIntegrationTestInfoMessage, "sparklogs.IntegrationTests.InfoMessage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginIntegrationTestInfoMessage::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginIntegrationTestInfoMessage::RunTest(const FString& Parameters)
{
    FScopedValueSetter<ELogTimes::Type> DoNotPrintTimes(GPrintLogTimes, ELogTimes::None);
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    
    FsparklogsOutputDeviceFile OutputDevice(*TestLogFile, Streamer);
    OutputDevice.SetSuppressEventTag(false);
    // Newlines of any type get converted into a single \n. Newlines at the start or end get trimmed.
    OutputDevice.Log(TEXT("hello world"));
    OutputDevice.Flush();
    OutputDevice.TearDown();

    ExpectedPayloads.Add(TEXT("[{\"severity\": \"Info\",\"message\":\"hello world\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginIntegrationTestInfoMessageNoTags, "sparklogs.IntegrationTests.InfoMessageNoTags", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginIntegrationTestInfoMessageNoTags::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginIntegrationTestInfoMessageNoTags::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);

    FsparklogsOutputDeviceFile OutputDevice(*TestLogFile, Streamer);
    OutputDevice.SetSuppressEventTag(true);
    // Newlines of any type get converted into a single \n. Newlines at the start or end get trimmed.
    OutputDevice.Serialize(TEXT("hello world"), ELogVerbosity::Warning, NAME_None);
    OutputDevice.Flush();
    OutputDevice.TearDown();

    ExpectedPayloads.Add(TEXT("[{\"severity\": \"Warning\",\"message\":\"hello world\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginIntegrationTestMultiline, "sparklogs.IntegrationTests.Multiline", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginIntegrationTestMultiline::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginIntegrationTestMultiline::RunTest(const FString& Parameters)
{
    FScopedValueSetter<ELogTimes::Type> DoNotPrintTimes(GPrintLogTimes, ELogTimes::None);
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    
    FsparklogsOutputDeviceFile OutputDevice(*TestLogFile, Streamer);
    OutputDevice.SetSuppressEventTag(false);
    // Newlines of any type get converted into a single \n. Newlines at the start or end get trimmed.
    OutputDevice.Serialize(TEXT("Line 1\r\nSecond line is longer\n3\r\n   fourth line    \t\r\n"), ELogVerbosity::Warning, NAME_None);
    OutputDevice.Flush();
    OutputDevice.TearDown();

    ExpectedPayloads.Add(TEXT("[{\"message\":\"Warning: Line 1\\nSecond line is longer\\n3\\n   fourth line    \\t\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginIntegrationTestAddRawEvent, "sparklogs.IntegrationTests.AddRawEvent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginIntegrationTestAddRawEvent::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginIntegrationTestAddRawEvent::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);
    
    FsparklogsOutputDeviceFile OutputDevice(*TestLogFile, Streamer);
    // Newlines of any type get converted into a single \n. Newlines at the start or end get trimmed.
    OutputDevice.AddRawEvent(TEXT(""), TEXT("hello world"));
    OutputDevice.AddRawEvent(nullptr, TEXT("second event"));
    OutputDevice.AddRawEvent(nullptr, TEXT("first line\nsecond line\nthird line\nfourth line"));
    // all blank lines at the end are trimmed, blank lines at the start are not trimmed...
    OutputDevice.AddRawEvent(nullptr, TEXT("\n\n\ntrim test\n \n\n"));
    OutputDevice.AddRawEvent(TEXT("\"custom_field\": \"string\""), TEXT(""));
    OutputDevice.AddRawEvent(TEXT("\"custom_field\": \"string\", \"f2\": {\"int\": 25, \"b\": true}"), nullptr);
    OutputDevice.AddRawEvent(TEXT("\"f3\": \"v1\", \"f4\": \"v2\""), TEXT("combined test"));
    OutputDevice.Flush();
    OutputDevice.TearDown();

    FString Payload = TEXT("[");
    Payload += TEXT("{\"message\":\"hello world\"}");
    Payload += TEXT(",{\"message\":\"second event\"}");
    Payload += TEXT(",{\"message\":\"first line\\nsecond line\\nthird line\\nfourth line\"}");
    Payload += TEXT(",{\"message\":\"\\n\\n\\ntrim test\\n \"}");
    Payload += TEXT(",{\"custom_field\": \"string\",\"message\":\"\"}");
    Payload += TEXT(",{\"custom_field\": \"string\", \"f2\": {\"int\": 25, \"b\": true},\"message\":\"\"}");
    Payload += TEXT(",{\"f3\": \"v1\", \"f4\": \"v2\",\"message\":\"combined test\"}");
    Payload += TEXT("]");
    ExpectedPayloads.Add(Payload);
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginIntegrationTestAutoFlushLog, "sparklogs.IntegrationTests.AutoFlushLog", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginIntegrationTestAutoFlushLog::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginIntegrationTestAutoFlushLog::RunTest(const FString& Parameters)
{
    FScopedValueSetter<ELogTimes::Type> DoNotPrintTimes(GPrintLogTimes, ELogTimes::None);
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    constexpr double MinIntervalBetweenFlushes = 0.25;
    Settings->MinIntervalBetweenFlushes = MinIntervalBetweenFlushes;
    Settings->UnflushedBytesToAutoFlush = 128;
    // Make sure that without auto-flush we would not trigger a periodic processing of the log.
    Settings->ProcessingIntervalSecs = 1000.0;
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);

    FsparklogsOutputDeviceFile OutputDevice(*TestLogFile, Streamer);
    // Add a short message that will not trigger the auto flush...
    OutputDevice.Serialize(TEXT("hello world"), ELogVerbosity::Log, NAME_None);

    // Make sure that we have not processed any payloads yet
    FPlatformProcess::SleepNoStats(MinIntervalBetweenFlushes / 10.0);
    TestTrue(TEXT("FlushAndWait[BEFORE_AUTO_FLUSH] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));

    // Log something that is long enough to trigger auto-flush, but it will not trigger because it's not enough time yet...
    OutputDevice.Serialize(TEXT("123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789"), ELogVerbosity::Log, NAME_None);
    FPlatformProcess::SleepNoStats(MinIntervalBetweenFlushes / 10.0);
    TestTrue(TEXT("FlushAndWait[BEFORE_AUTO_FLUSH_TOO_SOON] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));

    // Now wait for the minimum interval and trigger a short message that should now trigger auto-flush
    FPlatformProcess::SleepNoStats(MinIntervalBetweenFlushes);
    OutputDevice.Serialize(TEXT("fin"), ELogVerbosity::Log, NAME_None);

    FString Payload = TEXT("[");
    Payload += TEXT("{\"severity\": \"Info\",\"message\":\"hello world\"}");
    Payload += TEXT(",{\"severity\": \"Info\",\"message\":\"123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789\"}");
    Payload += TEXT(",{\"severity\": \"Info\",\"message\":\"fin\"}");
    Payload += TEXT("]");
    ExpectedPayloads.Add(Payload);

    FPlatformProcess::SleepNoStats(0.5);
    TestTrue(TEXT("FlushAndWait[AFTER_AUTO_FLUSH] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));

    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FsparklogsPluginIntegrationTestAutoFlushRawEvent, "sparklogs.IntegrationTests.AutoFlushRawEvent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
void FsparklogsPluginIntegrationTestAutoFlushRawEvent::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
    SetupCompressionModes(OutBeautifiedNames, OutTestCommands);
}
bool FsparklogsPluginIntegrationTestAutoFlushRawEvent::RunTest(const FString& Parameters)
{
    int TestInstanceIndex = FMath::RandRange(1000, 10000);
    FITLTestTempDirectory TempDir(ITLGetTestDir(), TestInstanceIndex);
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), *FString::Printf(TEXT("test-sparklogs-%d.log"), TestInstanceIndex));

    TArray<FString> ExpectedPayloads;

    TSharedRef<FsparklogsSettings> Settings(new FsparklogsSettings(TestInstanceIndex));
    Settings->IncludeCommonMetadata = false;
    Settings->CompressionMode = (ITLCompressionMode)FCString::Atoi(*Parameters);
    constexpr double MinIntervalBetweenFlushes = 0.25;
    Settings->MinIntervalBetweenFlushes = MinIntervalBetweenFlushes;
    Settings->UnflushedBytesToAutoFlush = 128;
    // Make sure that without auto-flush we would not trigger a periodic processing of the log.
    Settings->ProcessingIntervalSecs = 1000.0;
    TSharedRef<FsparklogsStoreInMemPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor(new FsparklogsStoreInMemPayloadProcessor());
    TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> Streamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(TestInstanceIndex, *TestLogFile, Settings, PayloadProcessor, 16 * 1024, FString(), FString(), nullptr));
    Streamer->SetWeakThisPtr(Streamer);

    FsparklogsOutputDeviceFile OutputDevice(*TestLogFile, Streamer);
    // Add a short message that will not trigger the auto flush...
    OutputDevice.AddRawEvent(TEXT(""), TEXT("hello world"));

    // Make sure that we have not processed any payloads yet
    FPlatformProcess::SleepNoStats(MinIntervalBetweenFlushes / 10.0);
    TestTrue(TEXT("FlushAndWait[BEFORE_AUTO_FLUSH] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));

    // Log something that is long enough to trigger auto-flush, but it will not trigger because it's not enough time yet...
    OutputDevice.AddRawEvent(TEXT(""), TEXT("123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789"));
    FPlatformProcess::SleepNoStats(MinIntervalBetweenFlushes / 10.0);
    TestTrue(TEXT("FlushAndWait[BEFORE_AUTO_FLUSH_TOO_SOON] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));

    // Now wait for the minimum interval and trigger a short message that should now trigger auto-flush
    FPlatformProcess::SleepNoStats(MinIntervalBetweenFlushes);
    OutputDevice.AddRawEvent(TEXT(""), TEXT("fin"));

    FString Payload = TEXT("[");
    Payload += TEXT("{\"message\":\"hello world\"}");
    Payload += TEXT(",{\"message\":\"123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789\"}");
    Payload += TEXT(",{\"message\":\"fin\"}");
    Payload += TEXT("]");
    ExpectedPayloads.Add(Payload);

    FPlatformProcess::SleepNoStats(0.5);
    TestTrue(TEXT("FlushAndWait[AFTER_AUTO_FLUSH] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));

    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}
