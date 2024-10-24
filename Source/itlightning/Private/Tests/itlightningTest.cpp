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
    TArray<FString> Payloads;
    virtual bool ProcessPayload(const uint8* JSONPayloadInUTF8, int PayloadLen) override
    {
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestSkipByteMarker, "itlightning.UnitTests.SkipByteMarker", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
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
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Now that we have a newline, it should flush and capture everything
    ExpectedPayloads.Add(FString(TEXT("[{\"message\":\"Hello world!!\"}]")));
    ITLWriteStringToFile(LogWriter, TEXT("\r\n"));
    LogWriter->Flush();
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestUnicode, "itlightning.UnitTests.Unicode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
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
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestTrue(TEXT("FlushAndWait[FINAL] should capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestMaxLineSize, "itlightning.UnitTests.MaxLineSize", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
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
    TestTrue(TEXT("FlushAndWait[1] should succeed"), Streamer->FlushAndWait(1, false, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[1] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[1] should NOT capture everything"), FlushedEverything);

    // Finish the line that is 1/2x the max line size, then 1x that is exactly the same, then another line that is a little over, then end with an incomplete line
    ITLWriteStringToFile(LogWriter, TEXT("\r\n12345678\r\n1234567812\r\n123"));
    LogWriter->Flush();
    ExpectedPayloads.Add(TEXT("[{\"message\":\"1234\"},{\"message\":\"12345678\"},{\"message\":\"12345678\"},{\"message\":\"12\"}]"));
    TestTrue(TEXT("FlushAndWait[FINAL] should succeed"), Streamer->FlushAndWait(2, true, 10.0, FlushedEverything));
    TestTrue(TEXT("FlushAndWait[FINAL] payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    TestFalse(TEXT("FlushAndWait[FINAL] should NOT capture everything"), FlushedEverything);

    Streamer.Reset();
    return true;
}
