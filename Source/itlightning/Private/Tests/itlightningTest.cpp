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
        FUTF8ToTCHAR Converter((const ANSICHAR*)JSONPayloadInUTF8, PayloadLen);
        Payloads.Add(FString(Converter.Length(), Converter.Get()));
        return true;
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FitlightningPluginUnitTestSkipByteMarker, "itlightning.UnitTests.SkipByteMarker", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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

bool FitlightningPluginUnitTestSkipByteMarker::RunTest(const FString& Parameters)
{
    FTempDirectory TempDir(ITLGetTestDir());
    FString TestLogFile = FPaths::Combine(TempDir.GetTempDir(), TEXT("test-itlightning.log"));
    
    TArray<FString> ExpectedPayloads;
    ExpectedPayloads.Add(FString(TEXT("Hello world!!")));

    TSharedRef<IFileHandle> LogWriter(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*TestLogFile, true, true));
    uint8 UTF8ByteOrderMark[3] = { 0xEF, 0xBB, 0xBF };
    LogWriter->Write(UTF8ByteOrderMark, 3);
    ITLWriteStringToFile(LogWriter, TEXT("Hello world!!"));
    LogWriter->Flush();

    TSharedRef<FitlightningSettings> Settings(new FitlightningSettings());
    TSharedRef<FitlightningStoreInMemPayloadProcessor> PayloadProcessor(new FitlightningStoreInMemPayloadProcessor());
    TUniquePtr<FitlightningReadAndStreamToCloud> Streamer = MakeUnique<FitlightningReadAndStreamToCloud>(*TestLogFile, Settings, PayloadProcessor, 16*1024);
    bool FlushedEverything=false;
    TestTrue(TEXT("FlushAndWait should succeed"), Streamer->FlushAndWait(2, true, 10.0, FlushedEverything));

    Streamer.Reset();
    TestTrue(TEXT("payloads should match"), ITLComparePayloads(this, PayloadProcessor->Payloads, ExpectedPayloads));
    return true;
}
