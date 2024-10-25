// Copyright (C) 2024 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

#include "Misc/AutomationTest.h"
#include "CoreTypes.h"
#include "Containers/UnrealString.h"
#include "GenericPlatform/GenericPlatform.h"
#include "UniquePtr.h"
#include "SharedPointer.h"
#include "Algo/Compare.h"
#include "itlightning.h"

class FTempDirectory
{
public:
    FTempDirectory(const FString& InDirectoryPath) : DirectoryPath(InDirectoryPath)
    {
        Created = false;
        if (!IFileManager::Get().DirectoryExists(*DirectoryPath))
        {
            Created = IFileManager::Get().MakeDirectory(*DirectoryPath);
        }
    }
    ~FTempDirectory()
    {
        if (Created)
        {
            IFileManager::Get().DeleteDirectory(*DirectoryPath, false, true);
        }
    }
    FString GetTempDir() { return DirectoryPath; }

private:
    bool Created;
    FString DirectoryPath;
};

static FString ITLGetTestDir()
{
    FString ParentDir = FPaths::EngineIntermediateDir();
    if (ParentDir.Len() <= 0) { ParentDir = FPaths::ProjectDir(); }
    if (ParentDir.Len() <= 0) { ParentDir = FPaths::EngineIntermediateDir(); }
    check(ParentDir.Len() > 0);
    FString TempDir = FPaths::CreateTempFilename(*ParentDir, TEXT("itl-test-"));
    check(TempDir.Len() > 0);
    return TempDir;
}

static void ITLWriteStringToFile(TSharedRef<IFileHandle> FileHandle, const TCHAR* Str)
{
    FTCHARToUTF8 Converter(Str);
    FileHandle->Write((const uint8*)(Converter.Get()), Converter.Length());
}

/** A payload processor that keeps the payload in memory. */
class FitlightningStoreInMemPayloadProcessor : public IitlightningPayloadProcessor
{
public:
    bool FailProcessing;
    TArray<FString> Payloads;
    FitlightningStoreInMemPayloadProcessor() : FailProcessing(false) { }
    virtual bool ProcessPayload(const uint8* JSONPayloadInUTF8, int PayloadLen, FitlightningReadAndStreamToCloud* Streamer) override
    {
        if (FailProcessing)
        {
            ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("TEST: forcefully failing processing of payload of length %d"), PayloadLen);
            return false;
        }
        Payloads.Add(ITLConvertUTF8(JSONPayloadInUTF8, PayloadLen));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestSkipByteMarker, "itlightning.UnitTests.SkipByteMarker", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
bool FitlightningPluginUnitTestSkipByteMarker::RunTest(const FString& Parameters)
{
    FTempDirectory TempDir(ITLGetTestDir());
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), TEXT("test-itlightning.log"));
    
    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    uint8 TestUTF8ByteOrderMark[3] = { 0xEF, 0xBB, 0xBF };
    LogWriter->Write(TestUTF8ByteOrderMark, 3);
    ITLWriteStringToFile(LogWriter, TEXT("Hello world!!"));
    LogWriter->Flush();

    TSharedRef<FitlightningSettings> Settings(new FitlightningSettings());
    Settings->IncludeCommonMetadata = false;
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor, 16*1024);
    bool FlushedEverything=false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Now that we have a newline, it should flush and capture everything
    ExpectedPayloads.Add(FString(TEXT("[{\"message\":\"Hello world!!\"}]")));
    ITLWriteStringToFile(LogWriter, TEXT("\r\n"));
    LogWriter->Flush();
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestSkipEmptyPayloads, "itlightning.UnitTests.SkipEmptyPayloads", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
bool FitlightningPluginUnitTestSkipEmptyPayloads::RunTest(const FString& Parameters)
{
    FTempDirectory TempDir(ITLGetTestDir());
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), TEXT("test-itlightning.log"));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    
    TSharedRef<FitlightningSettings> Settings(new FitlightningSettings());
    Settings->IncludeCommonMetadata = false;
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor, 16 * 1024);
    // Test completely empty file
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[1] should capture everything"), FlushedEverything);

    // Test one blank line
    ITLWriteStringToFile(LogWriter, TEXT("\n"));
    LogWriter->Flush();
    TestTrue(TEXT("FlushAndWait[2] should succeed"), Streamer->FlushAndWait(1, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[2] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[2] should capture everything"), FlushedEverything);

    // Test additional flushes without changes in log file
    TestTrue(TEXT("FlushAndWait[3] should succeed"), Streamer->FlushAndWait(5, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[3] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[3] should capture everything"), FlushedEverything);

    // Test several blank lines and a partial last line
    ITLWriteStringToFile(LogWriter, TEXT("\r\n\n\n\n\r\n\r\n    "));
    LogWriter->Flush();
    TestTrue(TEXT("FlushAndWait[4] should succeed"), Streamer->FlushAndWait(2, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[4] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[4] should NOT capture everything"), FlushedEverything);

    // Test flush after no change in partial last line
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[FINAL] should NOT capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestMultiline, "itlightning.UnitTests.Multiline", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
bool FitlightningPluginUnitTestMultiline::RunTest(const FString& Parameters)
{
    FTempDirectory TempDir(ITLGetTestDir());
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), TEXT("test-itlightning.log"));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("Line 1\r\nSecond line is longer\r\n3\r\n   fourth line    \t\r\n"));
    LogWriter->Flush();

    TSharedRef<FitlightningSettings> Settings(new FitlightningSettings());
    Settings->IncludeCommonMetadata = false;
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor, 16 * 1024);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"Line 1\"},{\"message\":\"Second line is longer\"},{\"message\":\"3\"},{\"message\":\"   fourth line    \\t\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestNewlines, "itlightning.UnitTests.Newlines", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
bool FitlightningPluginUnitTestNewlines::RunTest(const FString& Parameters)
{
    FTempDirectory TempDir(ITLGetTestDir());
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), TEXT("test-itlightning.log"));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("\t\n\n\r\n\nlinux\nskip\rslash\rR\n \r\n \n"));
    LogWriter->Flush();

    TSharedRef<FitlightningSettings> Settings(new FitlightningSettings());
    Settings->IncludeCommonMetadata = false;
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor, 16 * 1024);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"\\t\"},{\"message\":\"linux\"},{\"message\":\"skip\\rslash\\rR\"},{\"message\":\" \"},{\"message\":\" \"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestUnicode, "itlightning.UnitTests.Unicode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
bool FitlightningPluginUnitTestUnicode::RunTest(const FString& Parameters)
{
    FTempDirectory TempDir(ITLGetTestDir());
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), TEXT("test-itlightning.log"));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    constexpr TCHAR* TestPayload1 = TEXT("Hello world in 2 languages: こんにちは世界   你好，世界");
    ITLWriteStringToFile(LogWriter, *FString::Format(TEXT("{0}\r\n"), { TestPayload1 }));
    LogWriter->Flush();

    TSharedRef<FitlightningSettings> Settings(new FitlightningSettings());
    Settings->IncludeCommonMetadata = false;
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor, 16 * 1024);
    ExpectedPayloads.Add(FString::Format(TEXT("[{\"message\":\"{0}\"}]"), { TestPayload1 }));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestMaxLineSize, "itlightning.UnitTests.MaxLineSize", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
bool FitlightningPluginUnitTestMaxLineSize::RunTest(const FString& Parameters)
{
    FTempDirectory TempDir(ITLGetTestDir());
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), TEXT("test-itlightning.log"));

    TArray<FString> ExpectedPayloads;
    constexpr int MaxLineSize = 8;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    // One line that is 2x size of max line, then an unfinished line that is 1/2x size of max line
    ITLWriteStringToFile(LogWriter, TEXT("1234567812345678\r\n1234"));
    LogWriter->Flush();

    TSharedRef<FitlightningSettings> Settings(new FitlightningSettings());
    Settings->IncludeCommonMetadata = false;
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor, MaxLineSize);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"12345678\"},{\"message\":\"12345678\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Finish the line that is 1/2x the max line size, then 1x that is exactly the same, then another line that is a little over, then end with an incomplete line
    ITLWriteStringToFile(LogWriter, TEXT("\r\n12345678\r\n1234567812\r\n123"));
    LogWriter->Flush();
    ExpectedPayloads.Add(TEXT("[{\"message\":\"1234\"},{\"message\":\"12345678\"},{\"message\":\"12345678\"},{\"message\":\"12\"}]"));
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[FINAL] should NOT capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestMaxLineSizeUnicode, "itlightning.UnitTests.MaxLineSizeUnicode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
bool FitlightningPluginUnitTestMaxLineSizeUnicode::RunTest(const FString& Parameters)
{
    FTempDirectory TempDir(ITLGetTestDir());
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), TEXT("test-itlightning.log"));

    TArray<FString> ExpectedPayloads;
    // IMPORTANT: this is in *bytes* and we do not split a line in the middle of a Unicode character
    constexpr int MaxLineSize = 8;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    // One line that is 2x size of max line, then an unfinished line that is 1/2x size of max line
    // NOTE: π requires 2 bytes to encode in UTF-8
    ITLWriteStringToFile(LogWriter, TEXT("1234ππ5678π34\r\n1π4"));
    LogWriter->Flush();

    TSharedRef<FitlightningSettings> Settings(new FitlightningSettings());
    Settings->IncludeCommonMetadata = false;
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor, MaxLineSize);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"1234\"},{\"message\":\"ππ5678\"},{\"message\":\"π34\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Finish the line that is 1/2x the max line size, then 1x that is exactly the same, then another line that is a little over, then end with an incomplete line
    // NOTE: Ω takes 3 bytes to encode in UTF-8
    ITLWriteStringToFile(LogWriter, TEXT("\r\n123Ω78\r\n12345Ωπ\r\nΩ\r\n"));
    LogWriter->Flush();
    ExpectedPayloads.Add(TEXT("[{\"message\":\"1π4\"},{\"message\":\"123Ω78\"},{\"message\":\"12345\"},{\"message\":\"Ωπ\"},{\"message\":\"Ω\"}]"));
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestStopAndResume, "itlightning.UnitTests.StopAndResume", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
bool FitlightningPluginUnitTestStopAndResume::RunTest(const FString& Parameters)
{
    FTempDirectory TempDir(ITLGetTestDir());
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), TEXT("test-itlightning.log"));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("Line 1\r\nLine 2\r\n1234"));
    LogWriter->Flush();

    TSharedRef<FitlightningSettings> Settings(new FitlightningSettings());
    Settings->IncludeCommonMetadata = false;
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor, 16 * 1024);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"Line 1\"},{\"message\":\"Line 2\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1-FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1-FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1-FINAL] should NOT capture everything"), FlushedEverything);

    int64 ProgressMarker = 0;
    Streamer->ReadProgressMarker(ProgressMarker);
    TestEqual(TEXT("FlushAndWait[1-FINAL] progress marker should match"), ProgressMarker, (int64)16);
    Streamer.Reset();
    
    // When we resume, it should remember that we already processed the first two lines and not generate a new payload
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor2(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer2 = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor2, 16 * 1024);
    TArray<FString> ExpectedPayloads2;
    TestTrue(TEXT("FlushAndWait[2-1] should succeed"), Streamer2->FlushAndWait(2, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[2-1] payloads should match"), ITLComparePayloads(this, PayloadProcessor2->Payloads, ExpectedPayloads2));
    TestFalse(TEXT("FlushAndWait[2-1] should NOT capture everything"), FlushedEverything);

    // finish the partial line and make sure it gets captured and nothing else
    ITLWriteStringToFile(LogWriter, TEXT("Line 3\r\nLine 4\r\nlast line\r\n"));
    LogWriter->Flush();
    ExpectedPayloads2.Add(TEXT("[{\"message\":\"1234Line 3\"},{\"message\":\"Line 4\"},{\"message\":\"last line\"}]"));
    TestTrue(TEXT("FlushAndWait[2-FINAL] should succeed"), Streamer2->FlushAndWait(2, false, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[2-FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor2->Payloads, ExpectedPayloads2));
    TestTrue(TEXT("FlushAndWait[2-FINAL] should capture everything"), FlushedEverything);
    
    Streamer2.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestHandleLogRotation, "itlightning.UnitTests.HandleLogRotation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
bool FitlightningPluginUnitTestHandleLogRotation::RunTest(const FString& Parameters)
{
    FTempDirectory TempDir(ITLGetTestDir());
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), TEXT("test-itlightning.log"));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("123456789012345678901234567890\r\n"));
    LogWriter->Flush();

    TSharedRef<FitlightningSettings> Settings(new FitlightningSettings());
    Settings->IncludeCommonMetadata = false;
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor, 16 * 1024);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"123456789012345678901234567890\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(2, false, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[1] should capture everything"), FlushedEverything);
    int64 ProgressMarker = 0;
    Streamer->ReadProgressMarker(ProgressMarker);
    TestEqual(TEXT("FlushAndWait[1] progress marker should match"), ProgressMarker, (int64)32);

    // Simulate the log file being rotated and add a new log line to it
    LogWriter->Seek(0);
    LogWriter->Truncate(0);
    LogWriter->Flush();
    ITL_DBG_UE_LOG(LogPluginITLightning, Display, TEXT("Truncated logfile size to %ld. logfile='%s'"), IFileManager::Get().FileSize(*TestLogFile), *TestLogFile);
    TestEqual(TEXT("Logfile should now have 0 size"), IFileManager::Get().FileSize(*TestLogFile), (int64)0);
    ITLWriteStringToFile(LogWriter, TEXT("Line 2\r\n"));
    LogWriter->Flush();

    ExpectedPayloads.Add(TEXT("[{\"message\":\"Line 2\"}]"));
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(3, false, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);
    Streamer->ReadProgressMarker(ProgressMarker);
    TestEqual(TEXT("FlushAndWait[FINAL] progress marker should match"), ProgressMarker, (int64)8);
    
    Streamer.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestRetryDelay, "itlightning.UnitTests.RetryDelay", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
bool FitlightningPluginUnitTestRetryDelay::RunTest(const FString& Parameters)
{
    FTempDirectory TempDir(ITLGetTestDir());
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), TEXT("test-itlightning.log"));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("Line 1\r\nLine 2\r\n1234"));
    LogWriter->Flush();

    TSharedRef<FitlightningSettings> Settings(new FitlightningSettings());
    Settings->IncludeCommonMetadata = false;
    // Setup so that we process success requests very quickly, but delay a long time after a failure
    constexpr double TestProcessIntervalSecs = 0.1;
    constexpr double TestRetryIntervalSecs = 3.0;
    Settings->ProcessIntervalSecs = TestProcessIntervalSecs;
    Settings->RetryIntervalSecs = TestRetryIntervalSecs;
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor, 16 * 1024);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"Line 1\"},{\"message\":\"Line 2\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, TestProcessIntervalSecs * 5, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Setup more data to log, but simulate failure of the payload processor
    PayloadProcessor->FailProcessing = true;
    ITLWriteStringToFile(LogWriter, TEXT("Line 3\r\nLine 4"));
    LogWriter->Flush();
    TestFalse(TEXT("FlushAndWait[2] should fail because of failure to process"), Streamer->FlushAndWait(1, false, false, TestProcessIntervalSecs * 5, FlushedEverything));
    TestFalse(TEXT("FlushAndWait[2] should NOT capture everything"), FlushedEverything);

    // Make sure all manual flush requests have been processed
    FPlatformProcess::SleepNoStats(TestProcessIntervalSecs * 5);

    // Even though processing the payload will no longer fail, we have to wait longer before a retry is allowed
    PayloadProcessor->FailProcessing = false;
    TestFalse(TEXT("FlushAndWait[3] should fail because of timeout waiting for processing to happen again"), Streamer->FlushAndWait(1, false, false, TestProcessIntervalSecs * 5, FlushedEverything));
    TestFalse(TEXT("FlushAndWait[3] should NOT capture everything"), FlushedEverything);
    // Waiting longer than the retry interval should succeed
    ExpectedPayloads.Add(TEXT("[{\"message\":\"1234Line 3\"}]"));
    TestTrue(TEXT("FlushAndWait[4] should succeed because wait period is longer than retry interval"), Streamer->FlushAndWait(1, false, false, TestRetryIntervalSecs * 1.2, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[4] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[4] should NOT capture everything"), FlushedEverything);

    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[FINAL] should NOT capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestClearRetryTimer, "itlightning.UnitTests.ClearRetryTimer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CriticalPriority | EAutomationTestFlags::EngineFilter)
bool FitlightningPluginUnitTestClearRetryTimer::RunTest(const FString& Parameters)
{
    FTempDirectory TempDir(ITLGetTestDir());
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), TEXT("test-itlightning.log"));

    TArray<FString> ExpectedPayloads;

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    ITLWriteStringToFile(LogWriter, TEXT("Line 1\r\nLine 2\r\n1234"));
    LogWriter->Flush();

    TSharedRef<FitlightningSettings> Settings(new FitlightningSettings());
    Settings->IncludeCommonMetadata = false;
    // Setup so that we process success requests very quickly, but delay a long time after a failure
    constexpr double TestProcessIntervalSecs = 0.1;
    constexpr double TestRetryIntervalSecs = 3.0;
    Settings->ProcessIntervalSecs = TestProcessIntervalSecs;
    Settings->RetryIntervalSecs = TestRetryIntervalSecs;
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor, 16 * 1024);
    ExpectedPayloads.Add(TEXT("[{\"message\":\"Line 1\"},{\"message\":\"Line 2\"}]"));
    bool FlushedEverything = false;
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, false, TestProcessIntervalSecs * 5, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Setup more data to log, but simulate failure of the payload processor
    PayloadProcessor->FailProcessing = true;
    ITLWriteStringToFile(LogWriter, TEXT("Line 3\r\nLine 4"));
    LogWriter->Flush();
    TestFalse(TEXT("FlushAndWait[2] should fail because of failure to process"), Streamer->FlushAndWait(1, false, false, TestProcessIntervalSecs * 5, FlushedEverything));
    TestFalse(TEXT("FlushAndWait[2] should NOT capture everything"), FlushedEverything);

    // Make sure all manual flush requests have been processed
    FPlatformProcess::SleepNoStats(TestProcessIntervalSecs * 5);

    // Even though processing the payload will no longer fail, we have to wait longer before a retry is allowed.
    // HOWEVER, clear the retry timer in this attempt, so it should succeed immediately!
    PayloadProcessor->FailProcessing = false;
    ExpectedPayloads.Add(TEXT("[{\"message\":\"1234Line 3\"}]"));
    TestTrue(TEXT("FlushAndWait[3] should succeed because the retry timer was cleared"), Streamer->FlushAndWait(1, true, false, TestProcessIntervalSecs * 5, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[3] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[3] should NOT capture everything"), FlushedEverything);

    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, false, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[FINAL] should NOT capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}
