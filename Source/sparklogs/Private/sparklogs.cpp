// Copyright (C) 2024-2025 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

#include "sparklogs.h"
#include "CoreGlobals.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "Misc/AssertionMacros.h"
#include "Misc/EngineVersion.h"
#include "Misc/OutputDeviceFile.h"
#include "Misc/OutputDeviceHelper.h"
#include "Misc/SecureHash.h"
#include "ISettingsModule.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/ThreadManager.h"
#include "Runtime/Launch/Resources/Version.h"

#ifndef ALLOW_LOG_FILE
#define ALLOW_LOG_FILE 1
#endif

/*
#if UE_BUILD_SHIPPING
	#include "Compression/lz4.h"
#else
	#include "Trace/LZ4/lz4.c.inl"
#endif
*/
#define LZ4_NAMESPACE ITLLZ4
#include "Trace/LZ4/lz4.c.inl"
#undef LZ4_NAMESPACE


#define LOCTEXT_NAMESPACE "FsparklogsModule"

DEFINE_LOG_CATEGORY(LogPluginSparkLogs);

// =============== Globals ===============================================================================

constexpr int GMaxLineLength = 512 * 1024;

static uint8 UTF8ByteOrderMark[3] = {0xEF, 0xBB, 0xBF};
constexpr uint8 CharInternalNewline = 0x1E; // Control code: RS (Record Separator)
constexpr uint8 CharInternalJSONStart = 0x16; // Control code: SYN (Synchronous Idle)
constexpr uint8 CharInternalJSONEnd = 0x17; // Control code: ETB (End of Transmission Block)
static const TCHAR* StrCharInternalNewline = TEXT("\x1E");
static const TCHAR* StrCharInternalJSONStart = TEXT("\x16");
static const TCHAR* StrCharInternalJSONEnd = TEXT("\x17");

#if !NO_LOGGING
const FName SparkLogsCategoryName(LogPluginSparkLogs.GetCategoryName());
#else
const FName SparkLogsCategoryName(TEXT("LogPluginSparkLogs"));
#endif

FString ITLConvertUTF8(const void* Data, int Len)
{
	FUTF8ToTCHAR Converter((const ANSICHAR*)(Data), Len);
	return FString(Converter.Length(), Converter.Get());
}

bool ITLFStringTrimCharStartEndInline(FString& s, TCHAR c)
{
	int32 Start = 0, NewLength = s.Len();
	bool Removed = false;
	while (NewLength > 0)
	{
		if (s[NewLength - 1] != c)
		{
			break;
		}
		NewLength--;
		Removed = true;
	}
	for (Start = 0; Start < NewLength; Start++)
	{
		if (s[Start] != c)
		{
			break;
		}
		Removed = true;
	}
	if (Removed)
	{
#if (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
		s.MidInline(Start, NewLength - Start, EAllowShrinking::No);
#else
		s.MidInline(Start, NewLength - Start, false);
#endif
	}
	return Removed;
}

const TCHAR* ITLSeverityToString(ELogVerbosity::Type Verbosity)
{
	if (Verbosity == ELogVerbosity::Log)
	{
		return TEXT("Info");
	}
	return ToString(Verbosity);
}

void ITLGetOSPlatformVersion(FString& OutPlatform, FString& OutMajorVersion)
{
	FString OSPlatform, OSSubVersion;
	FPlatformMisc::GetOSVersions(OSPlatform, OSSubVersion);
	
	// Use only the platform name without any specifics after it.
	OSPlatform.TrimStartAndEndInline();
	int FirstSpace = OSPlatform.Find(TEXT(" "));
	if (FirstSpace != INDEX_NONE)
	{
#if (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
		OSPlatform.LeftInline(FirstSpace, EAllowShrinking::No);
#else
		OSPlatform.LeftInline(FirstSpace, false);
#endif
	}
	OSPlatform.ToLowerInline();

	// For raw version only use major/minor and nothing after it (second period and beyond)
	FString OSRawVersion = FPlatformMisc::GetOSVersion();
	OSRawVersion.TrimStartAndEndInline();
	int Period = OSRawVersion.Find(".");
	if (Period != INDEX_NONE)
	{
		Period = OSRawVersion.Find(".", ESearchCase::IgnoreCase, ESearchDir::FromStart, Period + 1);
		if (Period != INDEX_NONE)
		{
#if (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
			OSRawVersion.LeftInline(Period, EAllowShrinking::No);
#else
			OSRawVersion.LeftInline(Period, false);
#endif
		}
	}

	if (OSPlatform.IsEmpty())
	{
		OSPlatform = TEXT("unknown");
	}
	if (OSRawVersion.IsEmpty())
	{
		OSRawVersion = TEXT("?");
	}
	OutPlatform = OSPlatform;
	OutMajorVersion = OSRawVersion;
}

FString ITLGetNetworkConnectionType()
{
	ENetworkConnectionType Type = FPlatformMisc::GetNetworkConnectionType();
	switch (Type)
	{
		case ENetworkConnectionType::Unknown:		return TEXT("");
		case ENetworkConnectionType::None:			return TEXT("None");
		case ENetworkConnectionType::AirplaneMode:	return TEXT("AirplaneMode");
		case ENetworkConnectionType::Cell:			return TEXT("Cell");
		case ENetworkConnectionType::WiFi:			return TEXT("WiFi");
		case ENetworkConnectionType::Ethernet:		return TEXT("Ethernet");
		default:									return TEXT("");
	}
}

FString ITLGetUTCDateTimeAsRFC3339(const FDateTime& DT)
{
	return FString::Printf(
		TEXT("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ"),
		DT.GetYear(), DT.GetMonth(), DT.GetDay(),
		DT.GetHour(), DT.GetMinute(), DT.GetSecond(),
		DT.GetMillisecond()
	);
}

FDateTime ITLEmptyDateTime = FDateTime(0);

FDateTime ITLParseDateTime(const FString& TimeStr)
{
	int64 Ts = FCString::Atoi64(*TimeStr);
	if (Ts > 0)
	{
		return FDateTime(Ts);
	}
	else
	{
		return ITLEmptyDateTime;
	}
}

bool ITLIsMobilePlatform()
{
#if PLATFORM_IOS || PLATFORM_TVOS || PLATFORM_ANDROID
	return true;
#else
	return false;
#endif
}

FString ITLParseHttpResponseCookies(FHttpResponsePtr Response)
{
	FString AllCookies;
	const TArray<FString> AllHeaders = Response->GetAllHeaders();
	for (const FString& Header : AllHeaders)
	{
		FString HeaderName;
		FString HeaderValue;
		if (Header.Split(TEXT(":"), &HeaderName, &HeaderValue))
		{
			HeaderName.TrimStartAndEndInline();
			if (HeaderName.Equals(TEXT("Set-Cookie"), ESearchCase::IgnoreCase))
			{
				FString NextCookie, NextCookieParameters;
				if (!HeaderValue.Split(TEXT(";"), &NextCookie, &NextCookieParameters))
				{
					NextCookie = HeaderValue;
				}
				NextCookie.TrimStartAndEndInline();
				if (!NextCookie.IsEmpty())
				{
					if (!AllCookies.IsEmpty())
					{
						AllCookies += TEXT("; ");
					}
					AllCookies += NextCookie;
				}
			}
		}
	}
	return AllCookies;
}

FString ITLCalcUniqueFieldName(const TSharedPtr<FJsonObject> Object, const FString& BaseName, int HintStartingNum)
{
	if (HintStartingNum < 1)
	{
		HintStartingNum = 1;
	}
	for (int i=HintStartingNum; true; i++)
	{
		FString NextKey = FString::Printf(TEXT("%s%d"), *BaseName, i);
		if (!Object.IsValid() || !Object->HasField(NextKey))
		{
			return NextKey;
		}
	}
}

FString ITLSanitizeINIKeyName(const FString& In)
{
	FString SanitizedName(TEXT(""), In.Len() + 8);
	for (int i = 0; i < In.Len(); i++)
	{
		TCHAR C = In[i];
		if (FChar::IsAlnum(C) || FChar::IsUnderscore(C) || C == TEXT('.'))
		{
			// valid
		}
		else if (FChar::IsControl(C) || FChar::IsGraph(C) || FChar::IsLinebreak(C) || FChar::IsWhitespace(C) || FChar::IsPrint(C) || FChar::IsPunct(C))
		{
			C = TEXT('_');
		}
		// else char is considered valid (e.g., some other unicode character that should be OK in key names)
		SanitizedName += C;
	}
	return SanitizedName;
}

const TCHAR* INISectionForEditor = TEXT("Editor");
const TCHAR* INISectionForCommandlet = TEXT("Commandlet");
const TCHAR* INISectionForServer = TEXT("Server");
const TCHAR* INISectionForClient = TEXT("Client");

const TCHAR* GetITLLaunchConfiguration(bool ForINISection)
{
	if (GIsEditor)
	{
		return ForINISection ? INISectionForEditor : TEXT("editor");
	}
	else if (IsRunningCommandlet())
	{
		return ForINISection ? INISectionForCommandlet : TEXT("commandlet");
	}
	else if (IsRunningDedicatedServer())
	{
		return ForINISection ? INISectionForServer : TEXT("server");
	}
	else
	{
		return ForINISection ? INISectionForClient : TEXT("client");
	}
}

FString GetITLINISettingPrefix()
{
	const TCHAR* LaunchConfiguration = GetITLLaunchConfiguration(true);
	return FString(LaunchConfiguration);
}

template <typename T> T GetValueForLaunchConfiguration(T ServerValue, T EditorValue, T ClientValue, T OtherValue) {
	FString Config = GetITLLaunchConfiguration(true);
	if (Config == INISectionForServer)
	{
		return ServerValue;
	}
	else if (Config == INISectionForEditor)
	{
		return EditorValue;
	}
	else if (Config == INISectionForClient)
	{
		return ClientValue;
	}
	else
	{
		return OtherValue;
	}
}

FString GetITLLogFileName(const TCHAR* LogTypeName)
{
	const TCHAR* LaunchConfiguration = GetITLLaunchConfiguration(false);
	FString Name = FString(TEXT("sparklogs-"), FCString::Strlen(LaunchConfiguration) + FCString::Strlen(LogTypeName) + FCString::Strlen(TEXT("-.log")));
	Name.Append(LaunchConfiguration).Append(TEXT("-")).Append(LogTypeName).Append(TEXT(".log"));
	return Name;
}

FString GetITLPluginStateFilename()
{
	const TCHAR* LaunchConfiguration = GetITLLaunchConfiguration(false);
	FString Name = FString(TEXT("sparklogs-"), FCString::Strlen(LaunchConfiguration) + FCString::Strlen(TEXT("-state.ini")));
	Name.Append(LaunchConfiguration).Append(TEXT("-state.ini"));
	return Name;
}

class FITLLogOutputDeviceFileInitializer
{
public:
	TUniquePtr<FOutputDeviceFile> LogDevice;
	FString LogFilePath;
	bool InitLogDevice(const TCHAR* Filename)
	{
		if (!LogDevice)
		{
			FString ParentDir = FPaths::GetPath(FPaths::ConvertRelativePathToFull(FGenericPlatformOutputDevices::GetAbsoluteLogFilename()));
			LogFilePath = FPaths::Combine(ParentDir, Filename);
			LogDevice = MakeUnique<FOutputDeviceFile>(*LogFilePath, /*bDisableBackup*/true, /*bAppendIfExists*/true, /*bCreateWriterLazily*/true, [](const TCHAR* AbsPathname) {});
			return true;
		}
		else
		{
			return false;
		}
	}
};

class FITLSparkLogsLogOutputDeviceFileInitializer
{
public:
	TUniquePtr<FsparklogsOutputDeviceFile> LogDevice;
	FString LogFilePath;
	bool InitLogDevice(const TCHAR* Filename, TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> CloudStreamer)
	{
		if (!LogDevice)
		{
			FString ParentDir = FPaths::GetPath(FPaths::ConvertRelativePathToFull(FGenericPlatformOutputDevices::GetAbsoluteLogFilename()));
			LogFilePath = FPaths::Combine(ParentDir, Filename);
			LogDevice = MakeUnique<FsparklogsOutputDeviceFile>(*LogFilePath, CloudStreamer);
			return true;
		}
		else
		{
			return false;
		}
	}
};

FITLSparkLogsLogOutputDeviceFileInitializer& GetITLInternalGameLog(TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> CloudStreamer)
{
	static FITLSparkLogsLogOutputDeviceFileInitializer Singleton;
	FString LogFileName = GetITLLogFileName(TEXT("run"));
	Singleton.InitLogDevice(*LogFileName, CloudStreamer);
	return Singleton;
}

FITLLogOutputDeviceFileInitializer& GetITLInternalOpsLog()
{
	static FITLLogOutputDeviceFileInitializer Singleton;
	FString LogFileName = GetITLLogFileName(TEXT("ops"));
	if (Singleton.InitLogDevice(*LogFileName))
	{
		// The ops log should only contain logs about this plugin itself
		Singleton.LogDevice->IncludeCategory(SparkLogsCategoryName);
	}
	return Singleton;
}

bool ITLCompressData(ITLCompressionMode Mode, const uint8* InData, int InDataLen, TArray<uint8>& OutData)
{
	int32 CompressedBufSize = 0;
	int CompressedSize = 0;
	switch (Mode)
	{
	case ITLCompressionMode::LZ4:
		if (InDataLen > LZ4_MAX_INPUT_SIZE)
		{
			return false;
		}
		CompressedBufSize = (int32)ITLLZ4::LZ4_compressBound(InDataLen);
#if (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
		OutData.SetNumUninitialized(CompressedBufSize, EAllowShrinking::No);
#else
		OutData.SetNumUninitialized(CompressedBufSize, false);
#endif
		if (InDataLen <= 0)
		{
			// no-op
			return true;
		}
		CompressedSize = ITLLZ4::LZ4_compress_default((const char*)InData, (char*)OutData.GetData(), InDataLen, CompressedBufSize);
		if (CompressedSize <= 0)
		{
			return false;
		}
#if (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
		OutData.SetNumUninitialized(CompressedSize, EAllowShrinking::No);
#else
		OutData.SetNumUninitialized(CompressedSize, false);
#endif
		return true;
	case ITLCompressionMode::None:
#if (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
		OutData.SetNumUninitialized(0, EAllowShrinking::No);
#else
		OutData.SetNumUninitialized(0, false);
#endif
		OutData.Append(InData, (int32)InDataLen);
		return true;
	default:
		return false;
	}
}

bool ITLDecompressData(ITLCompressionMode Mode, const uint8* InData, int InDataLen, int InOriginalDataLen, TArray<uint8>& OutData)
{
	int DecompressedBytes = 0;
	switch (Mode)
	{
	case ITLCompressionMode::LZ4:
#if (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
		OutData.SetNumUninitialized(InOriginalDataLen, EAllowShrinking::No);
#else
		OutData.SetNumUninitialized(InOriginalDataLen, false);
#endif
		if (InOriginalDataLen <= 0)
		{
			// no-op
			return true;
		}
		DecompressedBytes = ITLLZ4::LZ4_decompress_safe((const char*)InData, (char*)OutData.GetData(), InDataLen, InOriginalDataLen);
		if (DecompressedBytes < 0)
		{
			return false;
		}
#if (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
		OutData.SetNumUninitialized(DecompressedBytes, EAllowShrinking::No);
#else
		OutData.SetNumUninitialized(DecompressedBytes, false);
#endif
		return true;
	case ITLCompressionMode::None:
#if (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
		OutData.SetNumUninitialized(0, EAllowShrinking::No);
#else
		OutData.SetNumUninitialized(0, false);
#endif
		OutData.Append(InData, (int32)InDataLen);
		return true;
	default:
		return false;
	}
}

SPARKLOGS_API FString ITLGenerateNewRandomID()
{
	FString A = FGuid::NewGuid().ToString(EGuidFormats::Base36Encoded);
	FString B = FGuid::NewGuid().ToString(EGuidFormats::Base36Encoded);
	return (A + B).ToLower();
}

SPARKLOGS_API FString ITLGenerateRandomAlphaNumID(int Length)
{
	const FString Charset = TEXT("abcdefghijklmnopqrstuvwxyz0123456789");
	FString Result;
	Result.Reserve(Length);
	for (int i = 0; i < Length; ++i)
	{
		Result.AppendChar(Charset[FMath::RandRange(0, Charset.Len() - 1)]);
	}
	return Result;
}

// =============== FsparklogsSettings ===============================================================================

FsparklogsSettings::FsparklogsSettings()
	: AnalyticsUserIDType(ITLAnalyticsUserIDType::DeviceID)
	, AnalyticsMobileAutoSessionStart(DefaultAnalyticsMobileAutoSessionStart)
	, AnalyticsMobileAutoSessionEnd(DefaultAnalyticsMobileAutoSessionEnd)
	, CollectAnalytics(true)
	, CollectLogs(false)
	, RequestTimeoutSecs(DefaultRequestTimeoutSecs)
	, ActivationPercentage(DefaultActivationPercentage)
	, BytesPerRequest(DefaultBytesPerRequest)
	, ProcessingIntervalSecs(DefaultServerProcessingIntervalSecs)
	, RetryIntervalSecs(DefaultRetryIntervalSecs)
	, UnflushedBytesToAutoFlush(DefaultUnflushedBytesToAutoFlush)
	, IncludeCommonMetadata(DefaultIncludeCommonMetadata)
	, DebugLogForAnalyticsEvents(DefaultServerDebugLogForAnalyticsEvents)
	, DebugLogRequests(DefaultDebugLogRequests)
	, AutoStart(DefaultAutoStart)
	, CompressionMode(ITLCompressionMode::Default)
	, AddRandomGameInstanceID(DefaultAddRandomGameInstanceID)
	, StressTestGenerateIntervalSecs(0.0)
	, StressTestNumEntriesPerTick(0)
	, CachedAnalyticsInstallTime(ITLEmptyDateTime)
	, CachedAnalyticsSessionNumber(0)
	, CachedAnalyticsTransactionNumber(0)
	, CachedAnalyticsLastEvent(ITLEmptyDateTime)
{
}

FString FsparklogsSettings::GetEffectiveHttpEndpointURI(const FString& OverrideHTTPEndpointURI)
{
	CloudRegion.TrimStartAndEndInline();
	HttpEndpointURI.TrimStartAndEndInline();
	if (OverrideHTTPEndpointURI.Len() > 0)
	{
		return OverrideHTTPEndpointURI;
	}
	if (HttpEndpointURI.Len() > 0)
	{
		return HttpEndpointURI;
	}
	FString CloudRegionLower = CloudRegion.ToLower();
	if (CloudRegionLower == TEXT("local"))
	{
		// Send to the local DEBUG container
		return TEXT("http://localhost:8082/ingest/v1");
	}
	else if (!CloudRegionLower.IsEmpty())
	{
		return FString::Format(TEXT("https://ingest-{0}.engine.sparklogs.app/ingest/v1"), { CloudRegionLower });
	}
	else
	{
		return FString();
	}
}

FString FsparklogsSettings::GetEffectiveAnalyticsUserID()
{
	FScopeLock WriteLock(&CachedCriticalSection);
	if (AnalyticsGameID.IsEmpty())
	{
		return FString();
	}
	if (!CachedAnalyticsUserID.IsEmpty())
	{
		return CachedAnalyticsUserID;
	}
	
	FString NewID;
	switch (AnalyticsUserIDType)
	{
	case ITLAnalyticsUserIDType::DeviceID:
		NewID = FPlatformMisc::GetDeviceId().ToLower();
		break;
	}
	
	// If another method hasn't given us a valid ID yet, then see if we have already saved a previously generated one, and if not, generate and save a new one.
	if (!IsValidDeviceID(NewID))
	{
		constexpr const TCHAR* UserIDKey = TEXT("AnalyticsUserID");
		NewID = GConfig->GetStr(ITL_CONFIG_SECTION_NAME, UserIDKey, GGameUserSettingsIni);
		NewID.TrimStartAndEndInline();
		if (NewID.IsEmpty())
		{
			NewID = ITLGenerateNewRandomID();
			GConfig->SetString(ITL_CONFIG_SECTION_NAME, UserIDKey, *NewID, GGameUserSettingsIni);
			GConfig->Flush(false, GGameUserSettingsIni);
		}
	}
	
	CachedAnalyticsUserID = NewID;
	// Whenever the user ID changes, the player ID must be recalculated...
	CachedAnalyticsPlayerID.Reset();
	return NewID;
}

FString FsparklogsSettings::GetEffectiveAnalyticsPlayerID()
{
	FScopeLock WriteLock1(&CachedCriticalSection);
	if (AnalyticsGameID.IsEmpty())
	{
		return FString();
	}
	if (!CachedAnalyticsPlayerID.IsEmpty())
	{
		return CachedAnalyticsPlayerID;
	}
	// Have to unlock the CS before we acquire it again to get/generate the UserID
	WriteLock1.Unlock();
	FString UserID = GetEffectiveAnalyticsUserID();
	FScopeLock WriteLock2(&CachedCriticalSection);
	FString NewID = CalculatePlayerID(AnalyticsGameID, UserID);
	CachedAnalyticsPlayerID = NewID;
	return NewID;
}

FString FsparklogsSettings::CalculatePlayerID(const FString& GameID, const FString& UserID)
{
	FTCHARToUTF8 Converted(*(GameID + TEXT(":") + UserID));
	uint8 Hash[20];
	FSHA1::HashBuffer(Converted.Get(), Converted.Length(), Hash);
	return BytesToHex(Hash, sizeof(Hash)).Left(32);
}

FDateTime FsparklogsSettings::GetEffectiveAnalyticsInstallTime()
{ 
	FScopeLock WriteLock1(&CachedCriticalSection);
	if (CachedAnalyticsInstallTime != ITLEmptyDateTime)
	{
		return CachedAnalyticsInstallTime;
	}
	constexpr const TCHAR* InstallTimeKey = TEXT("AnalyticsInstallTime");
	FString TimeStr = GConfig->GetStr(ITL_CONFIG_SECTION_NAME, InstallTimeKey, GGameUserSettingsIni);
	TimeStr.TrimStartAndEndInline();
	FDateTime InstallTime = ITLParseDateTime(TimeStr);
	if (InstallTime == ITLEmptyDateTime)
	{
		InstallTime = FDateTime::UtcNow();
		int64 Ts = InstallTime.GetTicks();
		TimeStr = FString::Printf(TEXT("%lld"), Ts);
		GConfig->SetString(ITL_CONFIG_SECTION_NAME, InstallTimeKey, *TimeStr, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	
	CachedAnalyticsInstallTime = InstallTime;
	return InstallTime;
}

void FsparklogsSettings::SetUserID(const TCHAR* UserID)
{
	FScopeLock WriteLock(&CachedCriticalSection);
	CachedAnalyticsUserID = UserID;
	CachedAnalyticsPlayerID.Reset();
}

int FsparklogsSettings::GetSessionNumber(bool Increment)
{
	constexpr const TCHAR* Key = TEXT("AnalyticsSessionNumber");
	bool Changed = false;
	FScopeLock WriteLock(&CachedCriticalSection);
	if (CachedAnalyticsSessionNumber <= 0)
	{
		if (!GConfig->GetInt(ITL_CONFIG_SECTION_NAME, Key, CachedAnalyticsSessionNumber, GGameUserSettingsIni) || CachedAnalyticsSessionNumber <= 0)
		{
			CachedAnalyticsSessionNumber = 1;
			Changed = true;
			Increment = false;
		}
	}
	if (Increment)
	{
		CachedAnalyticsSessionNumber++;
		Changed = true;
	}
	if (Changed)
	{
		GConfig->SetInt(ITL_CONFIG_SECTION_NAME, Key, CachedAnalyticsSessionNumber, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	return CachedAnalyticsSessionNumber;
}

int FsparklogsSettings::GetTransactionNumber(bool Increment)
{
	constexpr const TCHAR* Key = TEXT("AnalyticsTransactionNumber");
	bool Changed = false;
	FScopeLock WriteLock(&CachedCriticalSection);
	if (CachedAnalyticsTransactionNumber <= 0)
	{
		if (!GConfig->GetInt(ITL_CONFIG_SECTION_NAME, Key, CachedAnalyticsTransactionNumber, GGameUserSettingsIni) || CachedAnalyticsTransactionNumber <= 0)
		{
			CachedAnalyticsTransactionNumber = 1;
			Changed = true;
			Increment = false;
		}
	}
	if (Increment)
	{
		CachedAnalyticsTransactionNumber++;
		Changed = true;
	}
	if (Changed)
	{
		GConfig->SetInt(ITL_CONFIG_SECTION_NAME, Key, CachedAnalyticsTransactionNumber, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	return CachedAnalyticsTransactionNumber;
}

int FsparklogsSettings::GetAttemptNumber(const FString& EventID, bool Increment, bool DeleteAfter)
{
	FString MapKey = ITLSanitizeINIKeyName(EventID.ToLower());
	FString Key = TEXT("AnalyticsAttemptNumber_") + MapKey;
	bool Changed = false;
	FScopeLock WriteLock(&CachedCriticalSection);
	int CachedValue = 0;
	int* CachedValuePtr = CachedAnalyticsAttemptNumber.Find(MapKey);
	if (CachedValuePtr == nullptr)
	{
		if (!GConfig->GetInt(ITL_CONFIG_SECTION_NAME, *Key, CachedValue, GGameUserSettingsIni) || CachedValue <= 0)
		{
			CachedValue = 0;
			Changed = true;
		}
		CachedAnalyticsAttemptNumber.Emplace(MapKey, CachedValue);
	}
	else
	{
		CachedValue = *CachedValuePtr;
	}
	if (Increment)
	{
		CachedValue++;
		Changed = true;
	}
	if (DeleteAfter)
	{
		CachedAnalyticsAttemptNumber.Remove(MapKey);
		GConfig->RemoveKey(ITL_CONFIG_SECTION_NAME, *Key, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	else if (Changed)
	{
		CachedAnalyticsAttemptNumber.Emplace(MapKey, CachedValue);
		GConfig->SetInt(ITL_CONFIG_SECTION_NAME, *Key, CachedValue, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	return CachedValue;
}

FDateTime FsparklogsSettings::GetEffectiveLastWrittenAnalyticsEvent()
{
	if (CachedAnalyticsLastEvent != ITLEmptyDateTime)
	{
		return CachedAnalyticsLastEvent;
	}
	FString TimeStr = GConfig->GetStr(ITL_CONFIG_SECTION_NAME, AnalyticsLastWrittenKey, GGameUserSettingsIni);
	TimeStr.TrimStartAndEndInline();
	FDateTime LastEvent = ITLParseDateTime(TimeStr);
	CachedAnalyticsLastEvent = LastEvent;
	return LastEvent;
}

void FsparklogsSettings::MarkLastWrittenAnalyticsEvent()
{
	FDateTime Now = FDateTime::UtcNow();
	FDateTime KnownLastEvent = CachedAnalyticsLastEvent;
	if (KnownLastEvent != ITLEmptyDateTime)
	{
		FTimespan Interval = Now - KnownLastEvent;
		if (FMath::Abs(Interval.GetTotalSeconds()) < 15.0)
		{
			// Only update this timestamp every few seconds for efficiency
			return;
		}
	}
	CachedAnalyticsLastEvent = Now;
	int64 Ts = Now.GetTicks();
	FString TimeStr = FString::Printf(TEXT("%lld"), Ts);
	GConfig->SetString(ITL_CONFIG_SECTION_NAME, AnalyticsLastWrittenKey, *TimeStr, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

void FsparklogsSettings::MarkEndOfAnalyticsSession()
{
	CachedAnalyticsLastEvent = ITLEmptyDateTime;
	GConfig->RemoveKey(ITL_CONFIG_SECTION_NAME, AnalyticsLastWrittenKey, GGameUserSettingsIni);
	GConfig->RemoveKey(ITL_CONFIG_SECTION_NAME, AnalyticsLastSessionStarted, GGameUserSettingsIni);
	GConfig->RemoveKey(ITL_CONFIG_SECTION_NAME, AnalyticsLastSessionID, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

void FsparklogsSettings::MarkStartOfAnalyticsSession(const FString& SessionID, FDateTime SessionStarted)
{
	GConfig->SetString(ITL_CONFIG_SECTION_NAME, AnalyticsLastSessionID, *SessionID, GGameUserSettingsIni);
	FString SessionStartedStr = FString::Printf(TEXT("%lld"), (int64)SessionStarted.GetTicks());
	GConfig->SetString(ITL_CONFIG_SECTION_NAME, AnalyticsLastSessionStarted, *SessionStartedStr, GGameUserSettingsIni);
	FDateTime Now = FDateTime::UtcNow();
	FString NowStr = FString::Printf(TEXT("%lld"), (int64)Now.GetTicks());
	CachedAnalyticsLastEvent = Now;
	GConfig->SetString(ITL_CONFIG_SECTION_NAME, AnalyticsLastWrittenKey, *NowStr, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

void FsparklogsSettings::GetLastAnalyticsSessionStartInfo(FString& OutSessionID, FDateTime& OutSessionStarted)
{
	OutSessionID = GConfig->GetStr(ITL_CONFIG_SECTION_NAME, AnalyticsLastSessionID, GGameUserSettingsIni);
	FString TimeStr = GConfig->GetStr(ITL_CONFIG_SECTION_NAME, AnalyticsLastSessionStarted, GGameUserSettingsIni);
	TimeStr.TrimStartAndEndInline();
	OutSessionStarted = ITLParseDateTime(TimeStr);
}

void FsparklogsSettings::LoadSettings()
{
	FString Section = ITL_CONFIG_SECTION_NAME;
	FString SettingPrefix = GetITLINISettingPrefix();
	
	// Settings that are common across any launch configuration

	AnalyticsGameID = GConfig->GetStr(*Section, TEXT("AnalyticsGameID"), GEngineIni);

	FString AnalyticsUserIDTypeStr = GConfig->GetStr(*Section, TEXT("AnalyticsUserIDType"), GEngineIni).ToLower();
	if (AnalyticsUserIDTypeStr == AnalyticsUserIDTypeDeviceID)
	{
		AnalyticsUserIDType = ITLAnalyticsUserIDType::DeviceID;
	}
	else if (AnalyticsUserIDTypeStr == AnalyticsUserIDTypeGenerated)
	{
		AnalyticsUserIDType = ITLAnalyticsUserIDType::Generated;
	}
	else
	{
		if (AnalyticsUserIDTypeStr.Len() > 0)
		{
			UE_LOG(LogPluginSparkLogs, Warning, TEXT("Unknown AnalyticsUserIDType=%s, using default value of device_id..."), *AnalyticsUserIDTypeStr);
		}
		AnalyticsUserIDType = ITLAnalyticsUserIDType::DeviceID;
	}
	if (!GConfig->GetBool(*Section, TEXT("AnalyticsMobileAutoSessionStart"), AnalyticsMobileAutoSessionStart, GEngineIni))
	{
		AnalyticsMobileAutoSessionStart = DefaultAnalyticsMobileAutoSessionStart;
	}
	if (!GConfig->GetBool(*Section, TEXT("AnalyticsMobileAutoSessionEnd"), AnalyticsMobileAutoSessionEnd, GEngineIni))
	{
		AnalyticsMobileAutoSessionEnd = DefaultAnalyticsMobileAutoSessionEnd;
	}

	// Settings specific to this launch configuration

	if (!GConfig->GetBool(*Section, *(SettingPrefix + TEXT("CollectAnalytics")), CollectAnalytics, GEngineIni))
	{
		CollectAnalytics = GetValueForLaunchConfiguration(DefaultServerCollectAnalytics, DefaultEditorCollectAnalytics, DefaultClientCollectAnalytics, false);
	}
	if (!GConfig->GetBool(*Section, *(SettingPrefix + TEXT("CollectLogs")), CollectLogs, GEngineIni))
	{
		CollectLogs = GetValueForLaunchConfiguration(DefaultServerCollectLogs, DefaultEditorCollectLogs, DefaultClientCollectLogs, false);
	}

	CloudRegion = GConfig->GetStr(*Section, *(SettingPrefix + TEXT("CloudRegion")), GEngineIni);
	HttpEndpointURI = GConfig->GetStr(*Section, *(SettingPrefix + TEXT("HTTPEndpointURI")), GEngineIni);
	if (!GConfig->GetDouble(*Section, *(SettingPrefix + TEXT("RequestTimeoutSecs")), RequestTimeoutSecs, GEngineIni))
	{
		RequestTimeoutSecs = DefaultRequestTimeoutSecs;
	}

	AgentID = GConfig->GetStr(*Section, *(SettingPrefix + TEXT("AgentID")), GEngineIni);
	AgentAuthToken = GConfig->GetStr(*Section, *(SettingPrefix + TEXT("AgentAuthToken")), GEngineIni);
	HttpAuthorizationHeaderValue = GConfig->GetStr(*Section, *(SettingPrefix + TEXT("HTTPAuthorizationHeaderValue")), GEngineIni);
	
	FString StringActivationPercentage;
	GConfig->GetString(*Section, *(SettingPrefix + TEXT("ActivationPercentage")), StringActivationPercentage, GEngineIni);
	StringActivationPercentage.TrimStartAndEndInline();
	if (!GConfig->GetDouble(*Section, *(SettingPrefix + TEXT("ActivationPercentage")), ActivationPercentage, GEngineIni))
	{
		ActivationPercentage = DefaultActivationPercentage;
	}
	else
	{
		// If it was an empty string, treat as the default
		if (StringActivationPercentage.IsEmpty())
		{
			ActivationPercentage = DefaultActivationPercentage;
		}
	}

	if (!GConfig->GetInt(*Section, *(SettingPrefix + TEXT("BytesPerRequest")), BytesPerRequest, GEngineIni))
	{
		BytesPerRequest = DefaultBytesPerRequest;
	}
	if (!GConfig->GetDouble(*Section, *(SettingPrefix + TEXT("ProcessingIntervalSecs")), ProcessingIntervalSecs, GEngineIni))
	{
		ProcessingIntervalSecs = GetValueForLaunchConfiguration<double>(DefaultServerProcessingIntervalSecs, DefaultEditorProcessingIntervalSecs, DefaultClientProcessingIntervalSecs, 2);
	}
	if (!GConfig->GetDouble(*Section, *(SettingPrefix + TEXT("RetryIntervalSecs")), RetryIntervalSecs, GEngineIni))
	{
		RetryIntervalSecs = DefaultRetryIntervalSecs;
	}
	if (!GConfig->GetInt(*Section, *(SettingPrefix + TEXT("UnflushedBytesToAutoFlush")), UnflushedBytesToAutoFlush, GEngineIni))
	{
		UnflushedBytesToAutoFlush = DefaultUnflushedBytesToAutoFlush;
	}
	if (!GConfig->GetDouble(*Section, *(SettingPrefix + TEXT("MinIntervalBetweenFlushes")), MinIntervalBetweenFlushes, GEngineIni))
	{
		MinIntervalBetweenFlushes = DefaultMinIntervalBetweenFlushes;
	}

	if (!GConfig->GetBool(*Section, *(SettingPrefix + TEXT("IncludeCommonMetadata")), IncludeCommonMetadata, GEngineIni))
	{
		IncludeCommonMetadata = DefaultIncludeCommonMetadata;
	}
	if (!GConfig->GetBool(*Section, *(SettingPrefix + TEXT("DebugLogForAnalyticsEvents")), DebugLogForAnalyticsEvents, GEngineIni))
	{
		DebugLogForAnalyticsEvents = GetValueForLaunchConfiguration(DefaultServerDebugLogForAnalyticsEvents, DefaultEditorDebugLogForAnalyticsEvents, DefaultClientDebugLogForAnalyticsEvents, false);
	}
	if (!GConfig->GetBool(*Section, *(SettingPrefix + TEXT("DebugLogRequests")), DebugLogRequests, GEngineIni))
	{
		DebugLogRequests = DefaultDebugLogRequests;
	}
	if (!GConfig->GetBool(*Section, *(SettingPrefix + TEXT("AutoStart")), AutoStart, GEngineIni))
	{
		AutoStart = DefaultAutoStart;
	}
	if (!GConfig->GetBool(*Section, *(SettingPrefix + TEXT("AddRandomGameInstanceID")), AddRandomGameInstanceID, GEngineIni))
	{
		AddRandomGameInstanceID = DefaultAddRandomGameInstanceID;
	}

	FString CompressionModeStr = GConfig->GetStr(*Section, *(SettingPrefix + TEXT("CompressionMode")), GEngineIni).ToLower();
	if (CompressionModeStr == TEXT("lz4"))
	{
		CompressionMode = ITLCompressionMode::LZ4;
	}
	else if (CompressionModeStr == TEXT("none"))
	{
		CompressionMode = ITLCompressionMode::None;
	}
	else
	{
		if (CompressionModeStr.Len() > 0)
		{
			UE_LOG(LogPluginSparkLogs, Warning, TEXT("Unknown compression_mode=%s, using default mode instead..."), *CompressionModeStr);
		}
		CompressionMode = ITLCompressionMode::Default;
	}

	if (!GConfig->GetDouble(*Section, *(SettingPrefix + TEXT("StressTestGenerateIntervalSecs")), StressTestGenerateIntervalSecs, GEngineIni))
	{
		StressTestGenerateIntervalSecs = 0.0;
	}
	if (!GConfig->GetInt(*Section, *(SettingPrefix + TEXT("StressTestNumEntriesPerTick")), StressTestNumEntriesPerTick, GEngineIni))
	{
		StressTestNumEntriesPerTick = 0;
	}

	EnforceConstraints();

	// Clear out the previously cached values
	FScopeLock WriteLock(&CachedCriticalSection);
	CachedAnalyticsUserID.Reset();
	CachedAnalyticsPlayerID.Reset();
}

void FsparklogsSettings::EnforceConstraints()
{
	AgentID.TrimStartAndEndInline();
	AgentAuthToken.TrimStartAndEndInline();

	if (RequestTimeoutSecs < MinRequestTimeoutSecs)
	{
		RequestTimeoutSecs = MinRequestTimeoutSecs;
	}
	if (RequestTimeoutSecs > MaxRequestTimeoutSecs)
	{
		RequestTimeoutSecs = MaxRequestTimeoutSecs;
	}
	if (BytesPerRequest < MinBytesPerRequest)
	{
		BytesPerRequest = MinBytesPerRequest;
	}
	if (BytesPerRequest > MaxBytesPerRequest)
	{
		BytesPerRequest = MaxBytesPerRequest;
	}
	double MinProcessingIntervalSecs = GetValueForLaunchConfiguration<double>(MinServerProcessingIntervalSecs, MinEditorProcessingIntervalSecs, MinClientProcessingIntervalSecs, 60.0 * 5);
	if (ProcessingIntervalSecs < MinProcessingIntervalSecs)
	{
		ProcessingIntervalSecs = MinProcessingIntervalSecs;
	}
	if (RetryIntervalSecs < MinRetryIntervalSecs)
	{
		RetryIntervalSecs = MinRetryIntervalSecs;
	}
	if (RetryIntervalSecs > MaxRetryIntervalSecs)
	{
		RetryIntervalSecs = MaxRetryIntervalSecs;
	}
	if (UnflushedBytesToAutoFlush < MinUnflushedBytesToAutoFlush)
	{
		UnflushedBytesToAutoFlush = MinUnflushedBytesToAutoFlush;
	}
	if (MinIntervalBetweenFlushes < MinMinIntervalBetweenFlushes)
	{
		MinIntervalBetweenFlushes = MinMinIntervalBetweenFlushes;
	}
	if (StressTestGenerateIntervalSecs > 0 && StressTestNumEntriesPerTick < 1)
	{
		StressTestNumEntriesPerTick = 1;
	}
	AnalyticsGameID.TrimStartAndEndInline();
	if (AnalyticsGameID.IsEmpty())
	{
		if (CollectAnalytics)
		{
			CollectAnalytics = false;
			UE_LOG(LogPluginSparkLogs, Log, TEXT("Analytics collection will not activate until game ID is set. Check plugin settings."));
		}
	}
}

bool FsparklogsSettings::IsValidDeviceID(const FString& deviceId)
{
	// Test if a device ID is all zeros (or is some variant of "null")
	FString idForTesting = deviceId.ToLower();
	idForTesting.ReplaceInline(TEXT("null"), TEXT(""));
	idForTesting.ReplaceInline(TEXT("0"), TEXT(""));
	idForTesting.ReplaceInline(TEXT(" "), TEXT(""));
	idForTesting.ReplaceInline(TEXT(","), TEXT(""));
	idForTesting.ReplaceInline(TEXT(":"), TEXT(""));
	idForTesting.ReplaceInline(TEXT("_"), TEXT(""));
	idForTesting.ReplaceInline(TEXT("-"), TEXT(""));
	idForTesting.ReplaceInline(TEXT("/"), TEXT(""));
	// Ignore a well-known Android device ID that was common on one brand of old Android devices...
	idForTesting.ReplaceInline(TEXT("9774d56d682e549c"), TEXT(""));
	// We require an ID that has material information beyond zeros and null
	idForTesting.TrimStartAndEndInline();
	if (idForTesting.IsEmpty())
	{
		return false;
	}
	return true;
}

// =============== FsparklogsWriteNDJSONPayloadProcessor ===============================================================================

FsparklogsWriteNDJSONPayloadProcessor::FsparklogsWriteNDJSONPayloadProcessor(FString InOutputFilePath) : OutputFilePath(InOutputFilePath) { }

bool FsparklogsWriteNDJSONPayloadProcessor::ProcessPayload(TArray<uint8>& JSONPayloadInUTF8, int PayloadLen, int OriginalPayloadLen, ITLCompressionMode CompressionMode, TWeakPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> StreamerWeakPtr)
{
	TUniquePtr<IFileHandle> DebugJSONWriter;
	DebugJSONWriter.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*OutputFilePath, true, true));
	if (DebugJSONWriter == nullptr)
	{
		return false;
	}
	TArray<uint8> DecompressedData;
	if (!ITLDecompressData(CompressionMode, JSONPayloadInUTF8.GetData(), PayloadLen, OriginalPayloadLen, DecompressedData))
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("WriteNDJSONPayloadProcessor: failed to decompress data in payload: mode=%d, len=%d, original_len=%d"), (int)CompressionMode, PayloadLen, OriginalPayloadLen);
		return false;
	}
	if (!DebugJSONWriter->Write((const uint8*)DecompressedData.GetData(), DecompressedData.Num())
		|| !DebugJSONWriter->Write((const uint8*)("\r\n"), 2)
		|| !DebugJSONWriter->Flush())
	{
		return false;
	}
	DebugJSONWriter.Reset();
	return true;
}

// =============== FsparklogsWriteHTTPPayloadProcessor ===============================================================================

FsparklogsWriteHTTPPayloadProcessor::FsparklogsWriteHTTPPayloadProcessor(const TCHAR* InEndpointURI, const TCHAR* InAuthorizationHeader, double InTimeoutSecs, bool InLogRequests)
	: EndpointURI(InEndpointURI)
	, AuthorizationHeader(InAuthorizationHeader)
	, LogRequests(InLogRequests)
{
	SetTimeoutSecs(InTimeoutSecs);
}

void FsparklogsWriteHTTPPayloadProcessor::SetTimeoutSecs(double InTimeoutSecs)
{
	TimeoutMillisec.Set((int32)(InTimeoutSecs * 1000.0));
}

bool FsparklogsWriteHTTPPayloadProcessor::ProcessPayload(TArray<uint8>& JSONPayloadInUTF8, int PayloadLen, int OriginalPayloadLen, ITLCompressionMode CompressionMode, TWeakPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> StreamerWeakPtr)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FsparklogsWriteHTTPPayloadProcessor_ProcessPayload);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|BEGIN"));
	if (LogRequests)
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: BEGIN: len=%d, original_len=%d, timeout_millisec=%d"), PayloadLen, OriginalPayloadLen, (int)(TimeoutMillisec.GetValue()));
	}
	
	FThreadSafeBool RequestEnded(false);
	FThreadSafeBool RequestSucceeded(false);
	FThreadSafeBool RetryableFailure(true);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(*EndpointURI);
	HttpRequest->SetVerb(TEXT("POST"));
	SetHTTPTimezoneHeader(HttpRequest);
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=UTF-8"));
	HttpRequest->SetHeader(TEXT("Authorization"), *AuthorizationHeader);
	FString LocalCookieHeader = GetDataCookieHeader();
	if (LocalCookieHeader.Len() > 0)
	{
		HttpRequest->SetHeader(TEXT("Cookie"), LocalCookieHeader);
	}
	HttpRequest->SetHeader(TEXT("X-Calc-GeoIP"), TEXT("true"));
	HttpRequest->SetHeader(TEXT("X-Client-Clock-Utc-Now"), *FString::Printf(TEXT("%lld"), (int64)(FDateTime::UtcNow().ToUnixTimestamp())));
	HttpRequest->SetTimeout((double)(TimeoutMillisec.GetValue()) / 1000.0);
	switch (CompressionMode)
	{
	case ITLCompressionMode::LZ4:
		HttpRequest->SetHeader(TEXT("Content-Encoding"), TEXT("lz4-block"));
		HttpRequest->SetHeader(TEXT("X-Original-Content-Length"), FString::FromInt(OriginalPayloadLen));
		break;
	case ITLCompressionMode::None:
		// no special header to set
		break;
	default:
		UE_LOG(LogPluginSparkLogs, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: unknown compression mode %d"), (int)CompressionMode);
		return false;
	}
	HttpRequest->SetContent(JSONPayloadInUTF8);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|Headers and data prepared"));
	HttpRequest->OnRequestWillRetry().BindLambda([&](FHttpRequestPtr Request, FHttpResponsePtr Response, float SecondsToRetry)
		{
			if (Request.IsValid())
			{
				// Important that this header reflects the current time when we actually submit the request (in the future)
				HttpRequest->SetHeader(TEXT("X-Client-Clock-Utc-Now"), *FString::Printf(TEXT("%lld"), (int64)(FDateTime::UtcNow().ToUnixTimestamp() + (int64)SecondsToRetry)));
			}
		});

	HttpRequest->OnProcessRequestComplete().BindLambda([&](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
		{
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|OnProcessRequestComplete|BEGIN"));
			if (LogRequests)
			{
				if (Response.IsValid())
				{
					UE_LOG(LogPluginSparkLogs, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: RequestComplete: successful=%d, http_status=%d"), bWasSuccessful ? 1 : 0, (int)(Response->GetResponseCode()));
				}
				else
				{
					UE_LOG(LogPluginSparkLogs, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: RequestComplete: successful=%d, null_response_object"), bWasSuccessful ? 1 : 0);
				}
			}
			if (bWasSuccessful && Response.IsValid())
			{
				FString ResponseBody = Response->GetContentAsString();
				int32 ResponseCode = Response->GetResponseCode();
				if (EHttpResponseCodes::IsOk(ResponseCode))
				{
					// Remember any session affinity cookies for the next request...
					FString NewCookies = ITLParseHttpResponseCookies(Response);
					if (!NewCookies.IsEmpty())
					{
						SetDataCookieHeader(NewCookies);
					}
					// Mark that we've successfully processed the request...
					RequestSucceeded.AtomicSet(true);
				}
				else if (EHttpResponseCodes::TooManyRequests == ResponseCode || ResponseCode >= EHttpResponseCodes::ServerError)
				{
					UE_LOG(LogPluginSparkLogs, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: Retryable HTTP response: status=%d, msg=%s"), (int)ResponseCode, *ResponseBody.TrimStartAndEnd());
					// Clear any session affinity cookies in case that is part of the issue...
					SetDataCookieHeader(TEXT(""));
					RequestSucceeded.AtomicSet(false);
					RetryableFailure.AtomicSet(true);
				}
				else if (EHttpResponseCodes::BadRequest == ResponseCode)
				{
					// Something about this input was unable to be processed -- drop this input and pretend success so we can continue, but warn about it
					UE_LOG(LogPluginSparkLogs, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: HTTP response indicates input cannot be processed. Will skip this payload! status=%d, msg=%s"), (int)ResponseCode, *ResponseBody.TrimStartAndEnd());
					RequestSucceeded.AtomicSet(true);
				}
				else
				{
					UE_LOG(LogPluginSparkLogs, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: Non-Retryable HTTP response: status=%d, msg=%s"), (int)ResponseCode, *ResponseBody.TrimStartAndEnd());
					RequestSucceeded.AtomicSet(false);
					RetryableFailure.AtomicSet(false);
				}
			}
			else
			{
				TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> StreamerPtr = StreamerWeakPtr.Pin();
				if (StreamerPtr.IsValid())
				{
					UE_LOG(LogPluginSparkLogs, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: General HTTP request failure; will retry; retry_seconds=%.3lf"), StreamerPtr->WorkerGetRetrySecs());
				}
				RequestSucceeded.AtomicSet(false);
				RetryableFailure.AtomicSet(true);
			}

			// Signal that the request has finished (success or failure)
			RequestEnded.AtomicSet(true);
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|OnProcessRequestComplete|END|RequestEnded=%d"), RequestEnded ? 1 : 0);
		});

	// Start the HTTP request
	double StartTime = FPlatformTime::Seconds();
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|Starting to process request at time=%.3lf"), StartTime);
	if (!HttpRequest->ProcessRequest())
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: failed to initiate HttpRequest"));
		RequestSucceeded.AtomicSet(false);
		RetryableFailure.AtomicSet(true);
	}
	else
	{
		// Synchronously wait for the request to complete or fail
		SleepWaitingForHTTPRequest(HttpRequest, RequestEnded, RequestSucceeded, RetryableFailure, StartTime);
	}

	// If we had a non-retryable failure, then trigger this worker to stop
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|After request finished|RequestSucceeded=%d|RetryableFailure=%d"), RequestSucceeded ? 1 : 0, RetryableFailure ? 1 : 0);
	if (!RequestSucceeded && !RetryableFailure)
	{
		TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> StreamerPtr = StreamerWeakPtr.Pin();
		if (StreamerPtr.IsValid())
		{
			UE_LOG(LogPluginSparkLogs, Error, TEXT("HTTPPayloadProcessor::ProcessPayload: stopping log streaming service after non-retryable failure"));
			StreamerPtr->Stop();
		}
	}

	if (LogRequests)
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("HTTPPayloadProcessor::ProcessPayload: END: success=%d, can_retry=%d"), RequestSucceeded ? 1 : 0, RetryableFailure ? 1 : 0);
	}
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|END|RequestSucceeded=%d|RetryableFailure=%d"), RequestSucceeded ? 1 : 0, RetryableFailure ? 1 : 0);
	return RequestSucceeded;
}

void FsparklogsWriteHTTPPayloadProcessor::SetHTTPTimezoneHeader(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest)
{
	static FString TimezoneHeader(TEXT("X-Timezone"));
	static FString TimezoneHeaderValueUTC(TEXT("UTC"));
	if (GPrintLogTimes == ELogTimes::Local)
	{
		FTimespan LocalOffset = FDateTime::Now() - FDateTime::UtcNow();
		int32 TotalMinutes = FMath::RoundToInt(LocalOffset.GetTotalMinutes());
		int32 Hours = FMath::Abs(TotalMinutes) / 60;
		int32 Minutes = FMath::Abs(TotalMinutes) % 60;
		const TCHAR* Sign = (TotalMinutes >= 0) ? TEXT("+") : TEXT("-");
		FString TZOffset = FString::Printf(TEXT("UTC%s%02d:%02d"), Sign, Hours, Minutes);
		HttpRequest->SetHeader(TimezoneHeader, TZOffset);
	}
	else
	{
		// Assume UTC
		HttpRequest->SetHeader(TimezoneHeader, TimezoneHeaderValueUTC);
	}
}

bool FsparklogsWriteHTTPPayloadProcessor::SleepWaitingForHTTPRequest(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest, FThreadSafeBool& RequestEnded, FThreadSafeBool& RequestSucceeded, FThreadSafeBool& RetryableFailure, double StartTime)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FsparklogsWriteHTTPPayloadProcessor_SleepWaitingForHTTPRequest);
	while (!RequestEnded)
	{
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("HTTPPayloadProcessor::ProcessPayload|In loop waiting for request to end|RequestEnded=%d"), RequestEnded ? 1 : 0);
		// TODO: support cancellation in the future if we need to
		double CurrentTime = FPlatformTime::Seconds();
		double Elapsed = CurrentTime - StartTime;
		// It's possible the timeout has shortened while we've been waiting, so always use the current timeout value
		double Timeout = (double)(TimeoutMillisec.GetValue()) / 1000.0;
		if (Elapsed > Timeout)
		{
			UE_LOG(LogPluginSparkLogs, Warning, TEXT("HTTPPayloadProcessor::ProcessPayload: Timed out after %.3lf seconds; will retry..."), Elapsed);
			HttpRequest->CancelRequest();
			RequestSucceeded.AtomicSet(false);
			RetryableFailure.AtomicSet(true);
			return false;
		}
		// Capture stats in this sleep to ensure it is counted as an idle scope...
		FPlatformProcess::Sleep(0.1f);
	}
	return true;
}

FString FsparklogsWriteHTTPPayloadProcessor::GetDataCookieHeader()
{
	FScopeLock WriteLock(&DataCriticalSection);
	return DataCookieHeader;
}

void FsparklogsWriteHTTPPayloadProcessor::SetDataCookieHeader(const FString& Value)
{
	FScopeLock WriteLock(&DataCriticalSection);
	DataCookieHeader = Value;
}

// =============== FsparklogsStressGenerator ===============================================================================

FsparklogsStressGenerator::FsparklogsStressGenerator(TSharedRef<FsparklogsSettings> InSettings)
	: Settings(InSettings)
	, Thread(nullptr)
{
	check(FPlatformProcess::SupportsMultithreading());
	FString ThreadName = TEXT("SparkLogs_StressGenerator");
	FPlatformAtomics::InterlockedExchangePtr((void**)&Thread, FRunnableThread::Create(this, *ThreadName, 0, TPri_BelowNormal));
}

FsparklogsStressGenerator::~FsparklogsStressGenerator()
{
	if (Thread)
	{
		delete Thread;
	}
	Thread = nullptr;
}

bool FsparklogsStressGenerator::Init()
{
	return true;
}

uint32 FsparklogsStressGenerator::Run()
{
	double StressTestGenerateIntervalSecs = Settings->StressTestGenerateIntervalSecs;
	int StressTestNumEntriesPerTick = Settings->StressTestNumEntriesPerTick;
	UE_LOG(LogPluginSparkLogs, Log, TEXT("FsparklogsStressGenerator starting. StressTestGenerateIntervalSecs=%.3lf, StressTestNumEntriesPerTick=%d"), StressTestGenerateIntervalSecs, StressTestNumEntriesPerTick);
	while (StopRequestCounter.GetValue() == 0)
	{
		for (int i = 0; i < StressTestNumEntriesPerTick; i++)
		{
			UE_LOG(LogEngine, Log, TEXT("FsparklogsStressGenerator|Stress test message is being generated at platform_time=%.3lf, iteration=%d, 12345678901234567890123456789012345678901234567890 1234567890123456789012345678901234567890123456 100 12345678901234567890123456789012345678901234567890 1234567890123456789012345678901234567890123456 200 12345678901234567890123456789012345678901234567890 1234567890123456789012345678901234567890123456 300 12345678901234567890123456789012345678901234567890 1234567890123456789012345678901234567890123456 400"), FPlatformTime::Seconds(), i);
		}
		FPlatformProcess::SleepNoStats(StressTestGenerateIntervalSecs);
	}
	UE_LOG(LogPluginSparkLogs, Log, TEXT("FsparklogsStressGenerator stopped..."));
	return 0;
}

void FsparklogsStressGenerator::Stop()
{
	UE_LOG(LogPluginSparkLogs, Log, TEXT("FsparklogsStressGenerator requesting stop..."));
	StopRequestCounter.Increment();
}

// =============== FsparklogsReadAndStreamToCloud ===============================================================================

void FsparklogsReadAndStreamToCloud::ComputeCommonEventJSON(bool IncludeCommonMetadata, const FString& GameInstanceID, const TMap<FString, FString>* AdditionalAttributes)
{
	FString CommonEventJSON;

	if (IncludeCommonMetadata)
	{
		FString EffectiveComputerName;
		if (OverrideComputerName.IsEmpty())
		{
			EffectiveComputerName = FPlatformProcess::ComputerName();
		}
		else
		{
			EffectiveComputerName = OverrideComputerName;
		}

		CommonEventJSON.Appendf(TEXT("\"hostname\": %s, \"pid\": %d"), *EscapeJsonString(EffectiveComputerName), FPlatformProcess::GetCurrentProcessId());
		FString ProjectName = FApp::GetProjectName();
		if (ProjectName.Len() > 0 && ProjectName != "None")
		{
			CommonEventJSON.Appendf(TEXT(", \"app\": %s"), *EscapeJsonString(ProjectName));
		}
	}

	if (Settings->AddRandomGameInstanceID && GameInstanceID.Len() > 0)
	{
		if (!CommonEventJSON.IsEmpty())
		{
			CommonEventJSON.Append(TEXT(", "));
		}
		CommonEventJSON.Appendf(TEXT("\"game_instance_id\": %s"), *EscapeJsonString(GameInstanceID));
	}

	// If game_id is set we should always include it regardless, it's required for good analytics data.
	if (!Settings->AnalyticsGameID.IsEmpty())
	{
		if (!CommonEventJSON.IsEmpty())
		{
			CommonEventJSON.Append(TEXT(", "));
		}
		CommonEventJSON.Appendf(TEXT("\"game_id\": %s"), *EscapeJsonString(Settings->AnalyticsGameID));
	}

	if (nullptr != AdditionalAttributes && AdditionalAttributes->Num() > 0)
	{
		for (const TPair<FString, FString>& Pair : *AdditionalAttributes)
		{
			if (!CommonEventJSON.IsEmpty())
			{
				CommonEventJSON.Append(TEXT(","));
			}
			CommonEventJSON.Appendf(TEXT("%s:%s"), *EscapeJsonString(Pair.Key), *EscapeJsonString(Pair.Value));
		}
	}

	if (!CommonEventJSON.IsEmpty())
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Common event JSON computed. unreal_engine_common_event_data={%s}"), *CommonEventJSON);
		int64 CommonEventJSONLen = FTCHARToUTF8_Convert::ConvertedLength(*CommonEventJSON, CommonEventJSON.Len());
#if (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
		CommonEventJSONData.SetNum(0, EAllowShrinking::No);
#else
		CommonEventJSONData.SetNum(0, false);
#endif
		CommonEventJSONData.AddUninitialized(CommonEventJSONLen);
		FTCHARToUTF8_Convert::Convert((ANSICHAR*)CommonEventJSONData.GetData(), CommonEventJSONLen, *CommonEventJSON, CommonEventJSON.Len());
	}
}

FsparklogsReadAndStreamToCloud::FsparklogsReadAndStreamToCloud(const FString& InSourceLogFile, TSharedRef<FsparklogsSettings> InSettings, TSharedRef<IsparklogsPayloadProcessor, ESPMode::ThreadSafe> InPayloadProcessor, int InMaxLineLength, const FString& InOverrideComputerName, const FString& GameInstanceID, const TMap<FString, FString>* AdditionalAttributes)
	: Settings(InSettings)
	, PayloadProcessor(InPayloadProcessor)
	, SourceLogFile(InSourceLogFile)
	, MaxLineLength(InMaxLineLength)
	, OverrideComputerName(InOverrideComputerName)
	, Thread(nullptr)
	, WorkerShippedLogOffset(0)
	, WorkerMinNextFlushPlatformTime(0)
	, WorkerNumConsecutiveFlushFailures(0)
	, WorkerLastFailedFlushPayloadSize(0)
	, LastFlushPlatformTime(0)
	, BytesQueuedSinceLastFlush(0)
{
	if (FPlatformProperties::RequiresCookedData())
	{
		ProgressMarkerPath = GGameUserSettingsIni;
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Constructor|Cooked data is required, using game user settings INI for progress marker..."));
	}
	else
	{
		ProgressMarkerPath = FPaths::Combine(FPaths::GetPath(InSourceLogFile), GetITLPluginStateFilename());
#if ENGINE_MAJOR_VERSION >= 5
		ProgressMarkerPath = FConfigCacheIni::NormalizeConfigIniPath(ProgressMarkerPath);
#else
		FPaths::RemoveDuplicateSlashes(ProgressMarkerPath);
		ProgressMarkerPath = FPaths::CreateStandardFilename(ProgressMarkerPath);
#endif
		// In UE5 the INI file must exist before attempting to use it in INI functions
		if (!IFileManager::Get().FileExists(*ProgressMarkerPath))
		{
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Constructor|Creating empty INI for progress marker state|ProgressMarkerPath=%s"), *ProgressMarkerPath);
			FFileHelper::SaveStringToFile(TEXT(""), *ProgressMarkerPath);
		}
	}
	ComputeCommonEventJSON(Settings->IncludeCommonMetadata, GameInstanceID, AdditionalAttributes);

	WorkerBuffer.AddUninitialized(Settings->BytesPerRequest);
	int BufferSize = Settings->BytesPerRequest + 4096 + (Settings->BytesPerRequest / 10);
	WorkerNextPayload.AddUninitialized(BufferSize);
	WorkerNextEncodedPayload.AddUninitialized(BufferSize);
	check(MaxLineLength > 0);
	check(FPlatformProcess::SupportsMultithreading());
	FString ThreadName = FString::Printf(TEXT("SparkLogs_Reader_%s"), *FPaths::GetBaseFilename(InSourceLogFile));
	FPlatformAtomics::InterlockedExchangePtr((void**)&Thread, FRunnableThread::Create(this, *ThreadName, 0, TPri_BelowNormal));
}

FsparklogsReadAndStreamToCloud::~FsparklogsReadAndStreamToCloud()
{
	if (Thread)
	{
		delete Thread;
	}
	Thread = nullptr;
}

bool FsparklogsReadAndStreamToCloud::Init()
{
	return true;
}

uint32 FsparklogsReadAndStreamToCloud::Run()
{
	WorkerFullyCleanedUp.AtomicSet(false);
	ReadProgressMarker(WorkerShippedLogOffset);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Run|BEGIN|WorkerShippedLogOffset=%d"), (int)WorkerShippedLogOffset);
	// A pending flush will be processed before stopping
	while (StopRequestCounter.GetValue() == 0 || FlushRequestCounter.GetValue() > 0)
	{
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Run|In loop|WorkerLastFlushFailed=%d|FlushRequestCounter=%d"), WorkerLastFlushFailed ? 1 : 0, (int)FlushRequestCounter.GetValue());
		// Only allow manual flushes if we are not in a retry delay because the last operation failed.
		if (WorkerLastFlushFailed == false && FlushRequestCounter.GetValue() > 0)
		{
			int32 NewValue = FlushRequestCounter.Decrement();
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Run|Manual flush requested|FlushRequestCounter=%d"), (int)NewValue);
			WorkerDoFlush();
		}
		else if (FPlatformTime::Seconds() > WorkerMinNextFlushPlatformTime)
		{
			// If we are waiting on a manual flush, and the retry timer finally expired, it's OK to mark this attempt as processing it.
			if (FlushRequestCounter.GetValue() > 0)
			{
				int32 NewValue = FlushRequestCounter.Decrement();
				ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Run|Manual flush requested after retry timer expired|FlushRequestCounter=%d"), (int)NewValue);
			}
			else
			{
				ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Run|Periodic flush"));
			}
			WorkerDoFlush();
		}
		else
		{
			// More coarse-grained sleep, we don't need to wake up and do work very often
			FPlatformProcess::SleepNoStats(0.1f);
		}
	}
	WorkerFullyCleanedUp.AtomicSet(true);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Run|END"));
	return 0;
}

void FsparklogsReadAndStreamToCloud::Stop()
{
	int32 NewValue = StopRequestCounter.Increment();
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|Stop|StopRequestCounter=%d"), (int)NewValue);
}

bool FsparklogsReadAndStreamToCloud::AccrueWrittenBytes(int N)
{
	int64 Result = N + BytesQueuedSinceLastFlush.fetch_add((int64)N);
	if (Result >= Settings->UnflushedBytesToAutoFlush)
	{
		double Now = FPlatformTime::Seconds();
		if ((Now - LastFlushPlatformTime.load()) >= Settings->MinIntervalBetweenFlushes)
		{
			// Even though flush hasn't started, we will initiate it, so do not attempt more auto-flushes until the interval has passed again.
			BytesQueuedSinceLastFlush.store(0);
			LastFlushPlatformTime.store(Now);
			RequestFlush();
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|AccrueWrittenBytes|Automatically requesting a flush after the min interval and queued bytes..."));
			return true;
		}
	}
	return false;
}

bool FsparklogsReadAndStreamToCloud::RequestFlush()
{
	// If we've already requested a stop, a flush is impossible
	if (StopRequestCounter.GetValue() > 0)
	{
		return false;
	}
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|RequestFlush|Initiating flush by incrementing counter..."));
	FlushRequestCounter.Increment();
	return true;
}

bool FsparklogsReadAndStreamToCloud::FlushAndWait(int N, bool ClearRetryTimer, bool InitiateStop, bool OnMainGameThread, double TimeoutSec, bool& OutLastFlushProcessedEverything)
{
	OutLastFlushProcessedEverything = false;
	bool WasSuccessful = true;

	// If we've already requested a stop, a flush is impossible
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|StopRequestCounter=%d"), (int)StopRequestCounter.GetValue());
	if (StopRequestCounter.GetValue() > 0)
	{
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|stop already requested, exiting with false"));
		return false;
	}

	if (ClearRetryTimer)
	{
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Clearing retry timer..."));
		WorkerLastFlushFailed.AtomicSet(false);
	}

	for (int i = 0; i < N; i++)
	{
		int StartFlushSuccessOpCounter = FlushSuccessOpCounter.GetValue();
		int StartFlushOpCounter = FlushOpCounter.GetValue();
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Starting Loop|i=%d|N=%d|FlushSuccessOpCounter=%d|FlushOpCounter=%d"), (int)i, (int)N, (int)StartFlushSuccessOpCounter, (int)StartFlushOpCounter);
		FlushRequestCounter.Increment();
		// Last time around, we might initiate a stop
		if (InitiateStop && i == N-1)
		{
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Initiating stop..."));
			Stop();
		}
		double StartTime = FPlatformTime::Seconds();
		double LastTime = StartTime;
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Waiting for request to finish...|StartTime=%.3lf"), StartTime);
		while (FlushOpCounter.GetValue() == StartFlushOpCounter)
		{
			double Now = FPlatformTime::Seconds();
			if (Now - StartTime > TimeoutSec)
			{
				ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Timed out, returning false"));
				return false;
			}
			if (OnMainGameThread)
			{
				// HTTP requests and other things won't be processed unless we tick
				double DeltaTime = Now - LastTime;
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
#if ENGINE_MAJOR_VERSION >= 5
				FTSTicker::GetCoreTicker().Tick(DeltaTime);
#else
				FTicker::GetCoreTicker().Tick(DeltaTime);
#endif
				FThreadManager::Get().Tick();
				// NOTE: the game does not normally progress the frame count during shutdown, follow the same logic here
				// GFrameCounter++;
			}
			FPlatformProcess::SleepNoStats(OnMainGameThread ? 0.01f : 0.05f);
			LastTime = Now;
		}
		WasSuccessful = FlushSuccessOpCounter.GetValue() != StartFlushSuccessOpCounter;
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Finished waiting for request|WasSuccessful=%d|FlushSuccessOpCounter=%d|FlushOpCounter=%d"), WasSuccessful ? 1 : 0, (int)FlushSuccessOpCounter.GetValue(), (int)FlushOpCounter.GetValue());
	}
	if (WasSuccessful)
	{
		OutLastFlushProcessedEverything = LastFlushProcessedEverything;
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|LastFlushProcessedEverything=%d"), OutLastFlushProcessedEverything ? 1 : 0);
	}
	if (InitiateStop) {
		// Wait for the worker to fully stop, up to the timeout
		double StartTime = FPlatformTime::Seconds();
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Waiting for thread to stop...|StartTime=%.3lf"), StartTime);
		while (!WorkerFullyCleanedUp)
		{
			if (FPlatformTime::Seconds() - StartTime > TimeoutSec)
			{
				ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|Timed out waiting for thread to stop"));
				return false;
			}
			FPlatformProcess::SleepNoStats(0.01f);
		}
	}
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|FlushAndWait|END|WasSuccessful=%d"), WasSuccessful ? 1 : 0);
	return WasSuccessful;
}

bool FsparklogsReadAndStreamToCloud::ReadProgressMarker(int64& OutMarker)
{
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|ReadProgressMarker|inifile='%s'|BEGIN"), *ProgressMarkerPath);
	OutMarker = 0;
	double OutDouble = 0.0;
	bool WasDisabled = GConfig->AreFileOperationsDisabled();
	GConfig->EnableFileOperations();
	bool Result = GConfig->GetDouble(ITL_CONFIG_SECTION_NAME, ProgressMarkerValue, OutDouble, ProgressMarkerPath);
	if (WasDisabled)
	{
		GConfig->DisableFileOperations();
	}
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|ReadProgressMarker|inifile='%s'|Result=%s|Marker=%f"), *ProgressMarkerPath, Result ? TEXT("success") : TEXT("failure"), OutDouble);
	if (!Result)
	{
		OutDouble = 0.0;
	}
	// Precise to 52+ bits
	OutMarker = (int64)(OutDouble);
	return true;
}

bool FsparklogsReadAndStreamToCloud::WriteProgressMarker(int64 InMarker)
{
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WriteProgressMarker|inifile='%s'|Marker=%lld"), *ProgressMarkerPath, InMarker);
	// TODO: should we use the sqlite plugin instead, maybe it's not as much overhead as writing INI file each time?
	// Precise to 52+ bits
	bool WasDisabled = GConfig->AreFileOperationsDisabled();
	GConfig->EnableFileOperations();
	GConfig->SetDouble(ITL_CONFIG_SECTION_NAME, ProgressMarkerValue, (double)(InMarker), ProgressMarkerPath);
	GConfig->Flush(false, ProgressMarkerPath);
	if (WasDisabled)
	{
		GConfig->DisableFileOperations();
	}
	return true;
}

void FsparklogsReadAndStreamToCloud::DeleteProgressMarker()
{
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|DeleteProgressMarker|inifile='%s'"), *ProgressMarkerPath);
	bool WasDisabled = GConfig->AreFileOperationsDisabled();
	GConfig->EnableFileOperations();
	GConfig->RemoveKey(ITL_CONFIG_SECTION_NAME, ProgressMarkerValue, ProgressMarkerPath);
	GConfig->Flush(false, ProgressMarkerPath);
	if (WasDisabled)
	{
		GConfig->DisableFileOperations();
	}
}

bool FindFirstByte(const uint8* Haystack, uint8 Needle, int MaxToSearch, int& OutIndex)
{
	OutIndex = -1;
	for (const uint8* RESTRICT Data = Haystack, *RESTRICT End = Data + MaxToSearch; Data != End; ++Data)
	{
		if (*Data == Needle)
		{
			OutIndex = static_cast<int>(Data - Haystack);
			return true;
		}
	}
	return false;
}

/** Append UTF-8 encoded string data as an escaped JSON string value. Also convert the
  * special encoded newline (CharInternalNewline) to a real newline. */
void AppendUTF8AsEscapedJsonString(TITLJSONStringBuilder& Builder, const ANSICHAR* String, int N)
{
	ANSICHAR ControlFormatBuf[16];
	ANSICHAR OneCharBuf[2] = {0, 0};
	Builder.Append("\"");
	for (const ANSICHAR* RESTRICT Data = String, *RESTRICT End = Data + N; Data != End; ++Data)
	{
		switch (*Data)
		{
		case '\"':
			Builder.Append("\\\"", 2 /* string length */);
			break;
		case '\b':
			Builder.Append("\\b", 2 /* string length */);
			break;
		case '\t':
			Builder.Append("\\t", 2 /* string length */);
			break;
		case '\n':
			Builder.Append("\\n", 2 /* string length */);
			break;
		case static_cast<ANSICHAR>(CharInternalNewline):
			Builder.Append("\\n", 2 /* string length */);
			break;
		case '\f':
			Builder.Append("\\f", 2 /* string length */);
			break;
		case '\r':
			Builder.Append("\\r", 2 /* string length */);
			break;
		case '\\':
			Builder.Append("\\\\", 2 /* string length */);
			break;
		default:
			// Any character 0x20 and above can be included as-is
			if ((uint8)(*Data) >= static_cast<UTF8CHAR>(0x20))
			{
				OneCharBuf[0] = *Data;
				Builder.Append(OneCharBuf);
			}
			else
			{
				// Rare control character
				FCStringAnsi::Snprintf(ControlFormatBuf, sizeof(ControlFormatBuf), "\\u%04x", static_cast<int>(*Data));
				Builder.Append(ControlFormatBuf);
			}
		}
	}
	Builder.Append("\"");
}

bool FsparklogsReadAndStreamToCloud::WorkerReadNextPayload(int& OutNumToRead, int64& OutEffectiveShippedLogOffset, int64& OutRemainingBytes)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FsparklogsReadAndStreamToCloud_WorkerReadNextPayload);

	OutEffectiveShippedLogOffset = WorkerShippedLogOffset;

	// Re-open the file. UE doesn't contain cross-platform class that can stay open and refresh the filesize OR to read up to N (but maybe less than N bytes).
	// The only solution and stay within UE class library is to just re-open the file every flush request. This is actually quite fast on modern platforms.
	TUniquePtr<IFileHandle> WorkerReader;
	WorkerReader.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*SourceLogFile, true));
	if (WorkerReader == nullptr)
	{
		if (!FPaths::FileExists(SourceLogFile))
		{
			OutEffectiveShippedLogOffset = 0;
			OutNumToRead = 0;
			OutRemainingBytes = 0;
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerReadNextPayload|Logfile does not yet exist, nothing to read|logfile='%s'"), *SourceLogFile);
			return true;
		}
		else
		{
			UE_LOG(LogPluginSparkLogs, Warning, TEXT("STREAMER: Failed to open logfile='%s'"), *SourceLogFile);
			return false;
		}
	}
	int64 FileSize = WorkerReader->Size();
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerReadNextPayload|opened log file|last_offset=%ld|current_file_size=%ld|logfile='%s'"), OutEffectiveShippedLogOffset, FileSize, *SourceLogFile);
	if (OutEffectiveShippedLogOffset > FileSize)
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("STREAMER: Logfile reduced size, re-reading from start: new_size=%ld, previously_processed_to=%ld, logfile='%s'"), FileSize, OutEffectiveShippedLogOffset, *SourceLogFile);
		OutEffectiveShippedLogOffset = 0;
		// Don't force a retried read to use the same payload size as last time since the whole file has changed.
		WorkerLastFailedFlushPayloadSize = 0;
	}
	// Start at the last known shipped position, read as many bytes as possible up to the max buffer size, and capture log lines into a JSON payload
	WorkerReader->Seek(OutEffectiveShippedLogOffset);
	OutRemainingBytes = FileSize - OutEffectiveShippedLogOffset;
	OutNumToRead = (int)(FMath::Clamp<int64>(OutRemainingBytes, 0, (int64)(WorkerBuffer.Num())));
	if (WorkerLastFailedFlushPayloadSize > 0 && OutNumToRead > WorkerLastFailedFlushPayloadSize)
	{
		// Retried requests always use the same max payload size as last time,
		// so that any retry has the same data as last time and can be deduplicated in worst-case scenarios.
		// (e.g., an actual observed scenario where Unreal Engine HTTP plugin was sending requests successfully
		// but was not processing responses properly and instead timing them out...)
		OutNumToRead = WorkerLastFailedFlushPayloadSize;
	}
	if (OutNumToRead <= 0)
	{
		// We've read everything we possibly can already
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerReadNextPayload|Nothing more can be read|FileSize=%ld|EffectiveShippedLogOffset=%ld"), FileSize, OutEffectiveShippedLogOffset);
		return true;
	}

	uint8* BufferData = WorkerBuffer.GetData();
	if (!WorkerReader->Read(BufferData, OutNumToRead))
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("STREAMER: Failed to read data: offset=%ld, bytes=%ld, logfile='%s'"), OutEffectiveShippedLogOffset, OutNumToRead, *SourceLogFile);
		return false;
	}
#if ITL_INTERNAL_DEBUG_LOG_DATA == 1
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerReadNextPayload|read data into buffer|offset=%ld|data_len=%d|data=%s|logfile='%s'"), OutEffectiveShippedLogOffset, OutNumToRead, *ITLConvertUTF8(BufferData, OutNumToRead), *SourceLogFile);
#else
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerReadNextPayload|read data into buffer|offset=%ld|data_len=%d|logfile='%s'"), OutEffectiveShippedLogOffset, OutNumToRead, *SourceLogFile);
#endif
	return true;
}

bool FsparklogsReadAndStreamToCloud::WorkerBuildNextPayload(int NumToRead, int& OutCapturedOffset, int& OutNumCapturedLines)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FsparklogsReadAndStreamToCloud_WorkerBuildNextPayload);
	OutCapturedOffset = 0;
	const uint8* BufferData = WorkerBuffer.GetData();
	OutNumCapturedLines = 0;
	WorkerNextPayload.Reset();
	WorkerNextPayload.Append("[");
	int NextOffset = 0;
	while (NextOffset < NumToRead)
	{
		// Skip the UTF-8 byte order marker (always at the start of the file)
		if (0 == std::memcmp(BufferData + NextOffset, UTF8ByteOrderMark, sizeof(UTF8ByteOrderMark)))
		{
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|skipping UTF8 BOM|offset_before=%d|offset_after=%d"), NextOffset, NextOffset + sizeof(UTF8ByteOrderMark));
			NextOffset += sizeof(UTF8ByteOrderMark);
			OutCapturedOffset = NextOffset;
			continue;
		}
		// We only process whole lines. See if we can find the next end of line character.
		int RemainingBytes = NumToRead - NextOffset;
		int NumToSearch = FMath::Min(RemainingBytes, MaxLineLength);
		int FoundIndex = 0;
		int ExtraToSkip = 1; // skip over the \n char
		bool HaveLine = FindFirstByte(BufferData + NextOffset, static_cast<uint8>('\n'), NumToSearch, FoundIndex);
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|after newline search|NextOffset=%d|HaveLine=%d|NumToSearch=%d|FoundIndex=%d"), NextOffset, (int)HaveLine, NumToSearch, FoundIndex);
		if (!HaveLine && NumToSearch == MaxLineLength && RemainingBytes > NumToSearch)
		{
			// Even though we didn't find a line, break the line at the max length and process it
			// It's unsafe to break a line in the middle of a multi-byte UTF-8, so find a safe break point...
			ExtraToSkip = 0;
			FoundIndex = MaxLineLength - 1;
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|no newline found, search for safe breakpoint|NextOffset=%d|FoundIndex=%d"), NextOffset, FoundIndex);
			while (FoundIndex > 0)
			{
				if (*(BufferData + NextOffset + FoundIndex) >= 0x80)
				{
					FoundIndex--;
				}
				else
				{
					// include this non-multi-byte character and break here
					FoundIndex++;
					break;
				}
			}
			HaveLine = true;
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|found safe breakpoint|NextOffset=%d|FoundIndex=%d|ExtraToSkip=%d"), NextOffset, FoundIndex, ExtraToSkip);
		}
		if (!HaveLine)
		{
			// No more complete lines to process, this is enough for now
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|no more lines to process, break"));
			break;
		}
		// Trim newlines control characters of any kind at the end
		while (FoundIndex > 0)
		{
			// We expect the FoundIndex to be the *first* non-newline character, and ExtraToSkip set to the number of newline chars to skip.
			// Check if the previous character is a newline character, and if so, skip capturing it.
			uint8 c = *(BufferData + NextOffset + FoundIndex - 1);
			if (c == '\n' || c == '\r' || c == CharInternalNewline)
			{
				ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|character at NextOffset=%d, FoundIndex=%d is newline, will skip it"), NextOffset, FoundIndex);
				ExtraToSkip++;
				FoundIndex--;
			}
			else
			{
				break;
			}
		}
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|line summary|NextOffset=%d|FoundIndex=%d|ExtraToSkip=%d"), NextOffset, FoundIndex, ExtraToSkip);
		// Skip blank lines without capturing anything
		if (FoundIndex <= 0)
		{
			ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|skipping blank line..."));
			if (ExtraToSkip <= 0)
			{
				ExtraToSkip = 1;
			}
			NextOffset += ExtraToSkip;
			OutCapturedOffset = NextOffset;
			continue;
		}
		// Capture the data from (BufferData + NextOffset) to (BufferData + NextOffset + FoundIndex)
		// NOTE: the data in the logfile was already written in UTF-8 format
		if (OutNumCapturedLines > 0)
		{
			WorkerNextPayload.Append(",");
		}
		WorkerNextPayload.Append("{");
		if (CommonEventJSONData.Num() > 0)
		{
			WorkerNextPayload.Append((const ANSICHAR*)(CommonEventJSONData.GetData()), CommonEventJSONData.Num());
			WorkerNextPayload.Append(",");
		}
		// If we have raw JSON in the payload extract that portion and append it, setting up just the message text to remain for appending...
		if (FoundIndex > 2 && *(BufferData + NextOffset) == CharInternalJSONStart)
		{
			int FoundJSONEndIndex = 0;
			if (FindFirstByte(BufferData + NextOffset + 1, CharInternalJSONEnd, FoundIndex - 1, FoundJSONEndIndex))
			{
				WorkerNextPayload.Append((const ANSICHAR*)(BufferData + NextOffset + 1), FoundJSONEndIndex);
				if (FoundJSONEndIndex > 0)
				{
					WorkerNextPayload.Append(",");
				}
				NextOffset += (FoundJSONEndIndex + 2);
				FoundIndex -= (FoundJSONEndIndex + 2);
			}
		}
		WorkerNextPayload.Append("\"message\":", 10 /* length of `"message":` */);
		AppendUTF8AsEscapedJsonString(WorkerNextPayload, (const ANSICHAR*)(BufferData + NextOffset), FoundIndex);
#if ITL_INTERNAL_DEBUG_LOG_DATA == 1
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerBuildNextPayload|adding message to payload: %s"), *ITLConvertUTF8(BufferData + NextOffset, FoundIndex));
#endif
		WorkerNextPayload.Append("}");
		OutNumCapturedLines++;
		NextOffset += FoundIndex + ExtraToSkip;
		OutCapturedOffset = NextOffset;
	}
	WorkerNextPayload.Append("]");
	return true;
}

bool FsparklogsReadAndStreamToCloud::WorkerCompressPayload()
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FsparklogsReadAndStreamToCloud_WorkerCompressPayload);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerCompressPayload|Begin compressing payload"));
	bool Success = ITLCompressData(Settings->CompressionMode, (const uint8*)WorkerNextPayload.GetData(), WorkerNextPayload.Len(), WorkerNextEncodedPayload);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerCompressPayload|Finish compressing payload|success=%d|original_len=%d|compressed_len=%d"), Success ? 1 : 0, (int)WorkerNextPayload.Len(), (int)WorkerNextEncodedPayload.Num());
	return Success;
}

bool FsparklogsReadAndStreamToCloud::WorkerInternalDoFlush(int64& OutNewShippedLogOffset, bool& OutFlushProcessedEverything)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FsparklogsReadAndStreamToCloud_WorkerInternalDoFlush);
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerInternalDoFlush|BEGIN"));
	OutNewShippedLogOffset = WorkerShippedLogOffset;
	OutFlushProcessedEverything = false;
	LastFlushPlatformTime.store(FPlatformTime::Seconds());
	BytesQueuedSinceLastFlush.store(0);
	
	int NumToRead = 0;
	int64 EffectiveShippedLogOffset = WorkerShippedLogOffset, RemainingBytes;
	if (!WorkerReadNextPayload(NumToRead, EffectiveShippedLogOffset, RemainingBytes))
	{
		return false;
	}
	if (NumToRead <= 0)
	{
		// nothing more to read
		OutFlushProcessedEverything = true;
		return true;
	}
	
	int CapturedOffset = 0;
	int NumCapturedLines = 0;
	if (!WorkerBuildNextPayload(NumToRead, CapturedOffset, NumCapturedLines))
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("STREAMER: Failed to build payload: offset=%ld, payload_input_size=%d, logfile='%s'"), EffectiveShippedLogOffset, CapturedOffset, *SourceLogFile);
		return false;
	}

#if ITL_INTERNAL_DEBUG_LOG_DATA == 1
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerInternalDoFlush|payload is ready to process|offset=%ld|payload_input_size=%d|captured_lines=%d|data_len=%d|data=%s|logfile='%s'"),
		EffectiveShippedLogOffset, CapturedOffset, NumCapturedLines, WorkerNextPayload.Len(), *ITLConvertUTF8(WorkerNextPayload.GetData(), WorkerNextPayload.Len()), *SourceLogFile);
#else
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerInternalDoFlush|payload is ready to process|offset=%ld|payload_input_size=%d|captured_lines=%d|data_len=%d|logfile='%s'"),
		EffectiveShippedLogOffset, CapturedOffset, NumCapturedLines, WorkerNextPayload.Len(), *SourceLogFile);
#endif
	if (NumCapturedLines > 0)
	{
		if (!WorkerCompressPayload())
		{
			UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER: Failed to compress payload: mode=%d"), (int)Settings->CompressionMode);
			return false;
		}
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerInternalDoFlush|Begin processing payload"));
		if (!PayloadProcessor->ProcessPayload(WorkerNextEncodedPayload, WorkerNextEncodedPayload.Num(), WorkerNextPayload.Len(), Settings->CompressionMode, WeakThisPtr))
		{
			UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER: Failed to process payload: offset=%ld, num_read=%d, payload_input_size=%d, logfile='%s'"), EffectiveShippedLogOffset, NumToRead, CapturedOffset, *SourceLogFile);
			WorkerLastFailedFlushPayloadSize = NumToRead;
			return false;
		}
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerInternalDoFlush|Finished processing payload|PayloadInputSize=%ld"), CapturedOffset);
	}
	int ProcessedOffset = CapturedOffset;

	// If we processed everything up until the end of the file, we captured everything we can.
	OutNewShippedLogOffset = EffectiveShippedLogOffset + ProcessedOffset;
	if ((int64)(ProcessedOffset) >= RemainingBytes)
	{
		OutFlushProcessedEverything = true;
	}
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerInternalDoFlush|END|FlushProcessedEverything=%d"), OutFlushProcessedEverything);
	return true;
}

bool FsparklogsReadAndStreamToCloud::WorkerDoFlush()
{
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerDoFlush|BEGIN"));
	int64 ShippedNewLogOffset = 0;
	bool FlushProcessedEverything = false;
	bool Result = WorkerInternalDoFlush(ShippedNewLogOffset, FlushProcessedEverything);
	if (!Result)
	{
		WorkerLastFlushFailed.AtomicSet(true);
		WorkerMinNextFlushPlatformTime = FPlatformTime::Seconds() + WorkerGetRetrySecs();
		LastFlushProcessedEverything.AtomicSet(false);
		// Increment this counter after the retry interval is calculated
		WorkerNumConsecutiveFlushFailures++;
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerDoFlush|internal flush failed|WorkerMinNextFlushPlatformTime=%.3lf|NumConsecutiveFlushFailures=%d"), WorkerMinNextFlushPlatformTime, WorkerNumConsecutiveFlushFailures);
	}
	else
	{
		WorkerLastFlushFailed.AtomicSet(false);
		WorkerNumConsecutiveFlushFailures = 0;
		WorkerLastFailedFlushPayloadSize = 0;
		WorkerShippedLogOffset = ShippedNewLogOffset;
		WriteProgressMarker(ShippedNewLogOffset);
		WorkerMinNextFlushPlatformTime = FPlatformTime::Seconds() + Settings->ProcessingIntervalSecs;
		LastFlushProcessedEverything.AtomicSet(FlushProcessedEverything);
		FlushSuccessOpCounter.Increment();
		ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerDoFlush|internal flush succeeded|ShippedNewLogOffset=%d|WorkerMinNextFlushPlatformTime=%.3lf|FlushProcessedEverything=%d"), (int)ShippedNewLogOffset, WorkerMinNextFlushPlatformTime, FlushProcessedEverything ? 1 : 0);
	}
	FlushOpCounter.Increment();
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerDoFlush|END|Result=%d"), Result ? 1 : 0);
	return Result;
}

double FsparklogsReadAndStreamToCloud::WorkerGetRetrySecs()
{
	double RetrySecs = Settings->RetryIntervalSecs * (WorkerNumConsecutiveFlushFailures + 1);
	if (RetrySecs > Settings->MaxRetryIntervalSecs)
	{
		RetrySecs = Settings->MaxRetryIntervalSecs;
	}
	ITL_DBG_UE_LOG(LogPluginSparkLogs, Display, TEXT("STREAMER|WorkerGetRetrySecs=%.3lf"), RetrySecs);
	return RetrySecs;
}

// =============== FsparklogsOutputDeviceFile ===============================================================================

FsparklogsOutputDeviceFile::FsparklogsOutputDeviceFile(const TCHAR* InFilename, TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> CloudStreamer)
: Failed(false)
, ForceLogFlush(false)
, AsyncWriter(nullptr)
, WriterArchive(nullptr)
, CloudStreamerWeakPtr(CloudStreamer)
{
	Filename = InFilename;
	ForceLogFlush = FParse::Param(FCommandLine::Get(), TEXT("FORCELOGFLUSH"));
}

FsparklogsOutputDeviceFile::~FsparklogsOutputDeviceFile()
{
	TearDown();
}

void FsparklogsOutputDeviceFile::SetCloudStreamer(TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> CloudStreamer)
{
	CloudStreamerWeakPtr = TWeakPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(CloudStreamer);
}

void FsparklogsOutputDeviceFile::TearDown()
{
	if (AsyncWriter)
	{
		FAsyncWriter* DeletedAsyncWriter = AsyncWriter;
		AsyncWriter = nullptr;
		delete DeletedAsyncWriter;
	}
	if (WriterArchive)
	{
		FArchive* DeletedWriterArchive = WriterArchive;
		WriterArchive = nullptr;
		delete DeletedWriterArchive;
	}
	Filename = FString();
}

void FsparklogsOutputDeviceFile::Flush()
{
	if (AsyncWriter)
	{
		AsyncWriter->Flush();
	}
}

void FsparklogsOutputDeviceFile::Serialize(const TCHAR* Data, ELogVerbosity::Type Verbosity, const class FName& Category, const double Time)
{
	if (!ShouldLogCategory(Category) || Verbosity == ELogVerbosity::SetColor)
	{
		return;
	}

	static bool WithinCriticalError = false;
	if (GIsCriticalError && !WithinCriticalError)
	{
		WithinCriticalError = true;
		Serialize(Data, Verbosity, Category, Time);
		WithinCriticalError = false;
	}
	else
	{
		if (!AsyncWriter && !Failed)
		{
			CreateAsyncWriter();
		}
		if (AsyncWriter)
		{
			if (WithinCriticalError)
			{
				// Minimize unnecessary allocations or processing and just try to log while we possibly still can.
				FOutputDeviceHelper::FormatCastAndSerializeLine(*AsyncWriter, Data, Verbosity, Category, Time, bSuppressEventTag, bAutoEmitLineTerminator);
				// Do not accrue written bytes in this situation
			}
			else
			{
				// When writing to our internal log we transform the data in the following ways:
				// - Convert any multi-line log messages to use CharInternalNewline so that each log event is logged as a single line of text.
				// - Remove any completely blank lines at the start and end.
				// - Prepend extra JSON to explicitly specify the log verbosity (overrides what AutoExtract might detect, especially for "Log" level messages where UE does not explicitly log the severity).
				FString DataStr = Data;
				// Replace windows or linux style newlines with a single StrCharInternalNewline character.
				DataStr.ReplaceCharInline(TEXT('\n'), StrCharInternalNewline[0], ESearchCase::CaseSensitive);
				DataStr.ReplaceInline(TEXT("\r"), TEXT(""), ESearchCase::CaseSensitive);
				ITLFStringTrimCharStartEndInline(DataStr, StrCharInternalNewline[0]);
				FString ExtraJSON;
				if (Verbosity == ELogVerbosity::Log || !GPrintLogVerbosity || bSuppressEventTag)
				{
					ExtraJSON.Reserve(31);
					// Treat UE log severity as authoritative and make sure it's explicitly encoded if it's not already implicitly encoded in the log text.
					if (ExtraJSON.Len() > 0)
					{
						ExtraJSON += ", ";
					}
					ExtraJSON += "\"severity\": \"";
					ExtraJSON += ITLSeverityToString(Verbosity);
					ExtraJSON += "\"";
				}

				// Format the transformed log line instead of the original
				FsparklogsOutputDeviceFile::InternalAddMessageEvent(*AsyncWriter, ExtraJSON, *DataStr, Verbosity, Category, Time, bSuppressEventTag);
				AccrueWrittenBytes(DataStr.Len() + 32);
			}
			if (ForceLogFlush)
			{
				Flush();
			}
		}
	}
}

void FsparklogsOutputDeviceFile::Serialize(const TCHAR* Data, ELogVerbosity::Type Verbosity, const class FName& Category)
{
	Serialize(Data, Verbosity, Category, -1.0);
}

void FsparklogsOutputDeviceFile::InternalAddMessageEvent(FArchive& Output, FString& RawJSON, const TCHAR* Message, ELogVerbosity::Type Verbosity, const class FName& Category, const double Time, bool bSuppressEventTag)
{
	// In general, make logs follow Windows convention
#if PLATFORM_UNIX
	static const TCHAR* Terminator = TEXT("\r\n");
#else
	static const TCHAR* Terminator = LINE_TERMINATOR;
#endif // PLATFORM_UNIX
	static const int32 TerminatorLength = FCString::Strlen(Terminator);
	static const int32 ConvertedTerminatorLength = FTCHARToUTF8_Convert::ConvertedLength(Terminator, TerminatorLength);

	const int32 RawJSONLength = RawJSON.Len();
	int32 ConvertedRawJSONLength = FTCHARToUTF8_Convert::ConvertedLength(*RawJSON, RawJSONLength);
	if (ConvertedRawJSONLength > 0)
	{
		// Add room for the starting and ending marker characters
		ConvertedRawJSONLength += 2;
	}

	FString EventTag;
	if (!bSuppressEventTag)
	{
		EventTag = FOutputDeviceHelper::FormatLogLine(Verbosity, Category, nullptr, GPrintLogTimes, Time);
	}
	const int32 EventTagLength = EventTag.Len();
	const int32 ConvertedEventTagLength = FTCHARToUTF8_Convert::ConvertedLength(*EventTag, EventTagLength);

	const int32 MessageLength = (Message == nullptr) ? 0 : FCString::Strlen(Message);
	const int32 ConvertedMessageLength = (Message == nullptr) ? 0 : FTCHARToUTF8_Convert::ConvertedLength(Message, MessageLength);

	TArray<ANSICHAR, TInlineAllocator<2 * DEFAULT_STRING_CONVERSION_SIZE>> ConvertedEventData;
	ConvertedEventData.AddUninitialized(ConvertedRawJSONLength + ConvertedEventTagLength + ConvertedMessageLength + ConvertedTerminatorLength);
	if (ConvertedRawJSONLength > 0)
	{
		ConvertedEventData.GetData()[0] = CharInternalJSONStart;
		FTCHARToUTF8_Convert::Convert(ConvertedEventData.GetData() + 1, ConvertedRawJSONLength - 2, *RawJSON, RawJSONLength);
		ConvertedEventData.GetData()[ConvertedRawJSONLength - 1] = CharInternalJSONEnd;
	}
	if (ConvertedEventTagLength > 0)
	{
		FTCHARToUTF8_Convert::Convert(ConvertedEventData.GetData() + ConvertedRawJSONLength, ConvertedEventTagLength, *EventTag, EventTagLength);
	}
	if (ConvertedMessageLength > 0)
	{
		FTCHARToUTF8_Convert::Convert(ConvertedEventData.GetData() + ConvertedRawJSONLength + ConvertedEventTagLength, ConvertedMessageLength, Message, MessageLength);
	}
	FTCHARToUTF8_Convert::Convert(ConvertedEventData.GetData() + ConvertedRawJSONLength + ConvertedEventTagLength + ConvertedMessageLength, ConvertedTerminatorLength, Terminator, TerminatorLength);

	Output.Serialize(ConvertedEventData.GetData(), ConvertedEventData.Num() * sizeof(ANSICHAR));
}

bool FsparklogsOutputDeviceFile::AddRawEvent(const TCHAR* RawJSON, const TCHAR* Message)
{
	if (!AsyncWriter && !Failed)
	{
		CreateAsyncWriter();
	}
	if (!AsyncWriter)
	{
		return false;
	}

	if (RawJSON != nullptr && *RawJSON == 0)
	{
		RawJSON = nullptr;
	}
	if (Message != nullptr && *Message == 0)
	{
		Message = nullptr;
	}

	// In general, make logs follow Windows convention
#if PLATFORM_UNIX
	static const TCHAR* Terminator = TEXT("\r\n");
#else
	static const TCHAR* Terminator = LINE_TERMINATOR;
#endif // PLATFORM_UNIX
	static const int32 TerminatorLength = FCString::Strlen(Terminator);
	static const int32 ConvertedTerminatorLength = FTCHARToUTF8_Convert::ConvertedLength(Terminator, TerminatorLength);

	const int32 RawJSONLength = (RawJSON == nullptr) ? 0 : FCString::Strlen(RawJSON);
	int32 ConvertedRawJSONLength = (RawJSON == nullptr) ? 0 : FTCHARToUTF8_Convert::ConvertedLength(RawJSON, RawJSONLength);
	if (ConvertedRawJSONLength > 0)
	{
		// Add room for the starting and ending marker characters
		ConvertedRawJSONLength += 2;
	}

	const int32 MessageLength = (Message == nullptr) ? 0 : FCString::Strlen(Message);
	const int32 ConvertedMessageLength = (Message == nullptr) ? 0 : FTCHARToUTF8_Convert::ConvertedLength(Message, MessageLength);

	TArray<ANSICHAR, TInlineAllocator<2 * DEFAULT_STRING_CONVERSION_SIZE>> ConvertedEventData;
	ConvertedEventData.AddUninitialized(ConvertedRawJSONLength + ConvertedMessageLength + ConvertedTerminatorLength);
	if (ConvertedRawJSONLength > 0)
	{
		ConvertedEventData.GetData()[0] = CharInternalJSONStart;
		FTCHARToUTF8_Convert::Convert(ConvertedEventData.GetData() + 1, ConvertedRawJSONLength - 2, RawJSON, RawJSONLength);
		ConvertedEventData.GetData()[ConvertedRawJSONLength - 1] = CharInternalJSONEnd;
	}
	if (ConvertedMessageLength > 0)
	{
		FTCHARToUTF8_Convert::Convert(ConvertedEventData.GetData() + ConvertedRawJSONLength, ConvertedMessageLength, Message, MessageLength);
	}
	FTCHARToUTF8_Convert::Convert(ConvertedEventData.GetData() + ConvertedRawJSONLength + ConvertedMessageLength, ConvertedTerminatorLength, Terminator, TerminatorLength);

	// Any newline characters in the actual message must be replaced with the placeholder character to preserve multi-line log messages.
	ANSICHAR* EventDataBuffer = ConvertedEventData.GetData();
	for (int Index = ConvertedRawJSONLength; Index < (ConvertedRawJSONLength + ConvertedMessageLength); Index++)
	{
		if (EventDataBuffer[Index] == '\n')
		{
			EventDataBuffer[Index] = CharInternalNewline;
		}
	}

	AsyncWriter->Serialize(ConvertedEventData.GetData(), ConvertedEventData.Num() * sizeof(ANSICHAR));
	AccrueWrittenBytes(ConvertedEventData.Num() * sizeof(ANSICHAR));
	return true;
}

bool FsparklogsOutputDeviceFile::AddRawEventWithJSONObject(const FString& RawJSONWithBraces, const TCHAR* Message, bool AddUTCNow)
{
	FString TimestampFragment;
	if (AddUTCNow)
	{
		TimestampFragment = TEXT("\"timestamp\": \"") + ITLGetUTCDateTimeAsRFC3339(FDateTime::UtcNow()) + TEXT("\"");
	}
	if (RawJSONWithBraces.Len() > 2 && RawJSONWithBraces[0] == '{' && RawJSONWithBraces[RawJSONWithBraces.Len()-1] == '}')
	{
		TimestampFragment += TEXT(",");
		FString JSONWithoutBraces = TimestampFragment + RawJSONWithBraces.Mid(1, RawJSONWithBraces.Len() - 2);
		return AddRawEvent(*JSONWithoutBraces, Message);
	}
	else
	{
		return AddRawEvent(*TimestampFragment, Message);
	}
}

bool FsparklogsOutputDeviceFile::CreateAsyncWriter()
{
	if (IsOpened())
	{
		return true;
	}
	
	// Make sure it's a silent filewriter so we don't generate log messages on failure, generating an infinite feedback loop.
	uint32 Flags = FILEWRITE_Silent | FILEWRITE_AllowRead | FILEWRITE_Append;
	FArchive* Ar = IFileManager::Get().CreateFileWriter(*Filename, Flags);
	if (!Ar)
	{
		Failed = true;
		return false;
	}

	WriterArchive = Ar;
	AsyncWriter = new FAsyncWriter(*WriterArchive);
	// We always write UTF-8 to the logfile, make sure the file is interpreted properly as UTF-8
	AsyncWriter->Serialize((void*)UTF8ByteOrderMark, sizeof(UTF8ByteOrderMark));
	IFileManager::Get().SetTimeStamp(*Filename, FDateTime::UtcNow());
	Failed = false;
	return true;
}

bool FsparklogsOutputDeviceFile::ShouldLogCategory(const class FName& Category)
{
#if ALLOW_LOG_FILE && !NO_LOGGING
	return true;
#else
	return AlwaysLoggedCategories.Contains(Category);
#endif
}

bool FsparklogsOutputDeviceFile::AccrueWrittenBytes(int N)
{
	TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> CloudStreamer = CloudStreamerWeakPtr.Pin();
	if (CloudStreamer.IsValid())
	{
		if (CloudStreamer->AccrueWrittenBytes(N))
		{
			Flush();
			return true;
		}
	}
	return false;
}

// =============== FSparkLogsAnalyticsSessionDescriptor ===============================================================================

FSparkLogsAnalyticsSessionDescriptor::FSparkLogsAnalyticsSessionDescriptor()
	: SessionNumber(0)
	, SessionStarted(ITLEmptyDateTime)
{
}

FSparkLogsAnalyticsSessionDescriptor::FSparkLogsAnalyticsSessionDescriptor(const TCHAR* InSessionID, const TCHAR* InUserID)
	: SessionID(InSessionID)
	, SessionNumber(0)
	, SessionStarted(ITLEmptyDateTime)
	, UserID(InUserID)
{
}

FSparkLogsAnalyticsSessionDescriptor::FSparkLogsAnalyticsSessionDescriptor(const TCHAR* InSessionID, int InSessionNumber, const TCHAR* InUserID)
	: SessionID(InSessionID)
	, SessionNumber(InSessionNumber)
	, SessionStarted(ITLEmptyDateTime)
	, UserID(InUserID)
{
}

FSparkLogsAnalyticsSessionDescriptor::FSparkLogsAnalyticsSessionDescriptor(const TCHAR* InSessionID, int InSessionNumber, FDateTime InSessionStarted, const TCHAR* InUserID)
	: SessionID(InSessionID)
	, SessionNumber(InSessionNumber)
	, SessionStarted(InSessionStarted)
	, UserID(InUserID)
{
}

// =============== UsparklogsAnalytics ===============================================================================

UsparklogsAnalytics::UsparklogsAnalytics(const FObjectInitializer& OI) : Super(OI) { }

bool UsparklogsAnalytics::StartSession() { return FsparklogsModule::GetAnalyticsProvider()->StartSession(TArray<FAnalyticsEventAttribute>()); }
bool UsparklogsAnalytics::StartSessionWithReason(const FString& Reason) { return FsparklogsModule::GetAnalyticsProvider()->StartSession(*Reason, TArray<FAnalyticsEventAttribute>()); }
void UsparklogsAnalytics::EndSession() { FsparklogsModule::GetAnalyticsProvider()->EndSession(); }
void UsparklogsAnalytics::EndSessionWithReason(const FString& Reason) { FsparklogsModule::GetAnalyticsProvider()->EndSession(*Reason); }
FString UsparklogsAnalytics::GetSessionID() { return FsparklogsModule::GetAnalyticsProvider()->GetSessionID(); }
FSparkLogsAnalyticsSessionDescriptor UsparklogsAnalytics::GetSessionDescriptor() { return FsparklogsModule::GetAnalyticsProvider()->GetSessionDescriptor(); }
void UsparklogsAnalytics::SetBuildInfo(const FString& InBuildInfo) { FsparklogsModule::GetAnalyticsProvider()->SetBuildInfo(InBuildInfo); }
void UsparklogsAnalytics::SetCommonAttribute(const FString& Field, const FString& Value) { FsparklogsModule::GetAnalyticsProvider()->SetMetaAttribute(Field, TSharedPtr<FJsonValue>(new FJsonValueString(Value))); }
void UsparklogsAnalytics::SetCommonAttributeJSON(const FString& Field, const TSharedPtr<FJsonValue> Value) { FsparklogsModule::GetAnalyticsProvider()->SetMetaAttribute(Field, Value); }

bool UsparklogsAnalytics::AddPurchase(const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* RealCurrencyCode, double Amount, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventPurchase(ItemCategory, ItemId, RealCurrencyCode, Amount, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddPurchase(const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* RealCurrencyCode, double Amount, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventPurchase(ItemCategory, ItemId, RealCurrencyCode, Amount, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddPurchase(const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* RealCurrencyCode, double Amount, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventPurchase(ItemCategory, ItemId, RealCurrencyCode, Amount, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddPurchase(const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* RealCurrencyCode, double Amount, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventPurchase(ItemCategory, ItemId, RealCurrencyCode, Amount, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

void UsparklogsAnalytics::RecordPurchase(const FString& ItemCategory, const FString& ItemId, const FString& RealCurrencyCode, float Amount)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventPurchase(*ItemCategory, *ItemId, *RealCurrencyCode, Amount, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordPurchaseWithAttrs(const FString& ItemCategory, const FString& ItemId, const FString& RealCurrencyCode, float Amount, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventPurchase(*ItemCategory, *ItemId, *RealCurrencyCode, Amount, nullptr, CustomAttrs);
}

void UsparklogsAnalytics::RecordPurchaseWithReason(const FString& ItemCategory, const FString& ItemId, const FString& RealCurrencyCode, float Amount, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventPurchase(*ItemCategory, *ItemId, *RealCurrencyCode, Amount, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordPurchaseWithReasonWithAttrs(const FString& ItemCategory, const FString& ItemId, const FString& RealCurrencyCode, float Amount, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventPurchase(*ItemCategory, *ItemId, *RealCurrencyCode, Amount, *Reason, CustomAttrs);
}

bool UsparklogsAnalytics::AddResource(EsparklogsAnalyticsFlowType FlowType, double Amount, const TCHAR* VirtualCurrency, const TCHAR* ItemCategory, const TCHAR* ItemId, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventResource(FlowType, Amount, VirtualCurrency, ItemCategory, ItemId, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddResource(EsparklogsAnalyticsFlowType FlowType, double Amount, const TCHAR* VirtualCurrency, const TCHAR* ItemCategory, const TCHAR* ItemId, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventResource(FlowType, Amount, VirtualCurrency, ItemCategory, ItemId, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddResource(EsparklogsAnalyticsFlowType FlowType, double Amount, const TCHAR* VirtualCurrency, const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventResource(FlowType, Amount, VirtualCurrency, ItemCategory, ItemId, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddResource(EsparklogsAnalyticsFlowType FlowType, double Amount, const TCHAR* VirtualCurrency, const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventResource(FlowType, Amount, VirtualCurrency, ItemCategory, ItemId, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

void UsparklogsAnalytics::RecordResource(EsparklogsAnalyticsFlowType FlowType, float Amount, const FString& VirtualCurrency, const FString& ItemCategory, const FString& ItemId)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventResource(FlowType, Amount, *VirtualCurrency, *ItemCategory, *ItemId, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordResourceWithAttrs(EsparklogsAnalyticsFlowType FlowType, float Amount, const FString& VirtualCurrency, const FString& ItemCategory, const FString& ItemId, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventResource(FlowType, Amount, *VirtualCurrency, *ItemCategory, *ItemId, nullptr, CustomAttrs);
}

void UsparklogsAnalytics::RecordResourceWithReason(EsparklogsAnalyticsFlowType FlowType, float Amount, const FString& VirtualCurrency, const FString& ItemCategory, const FString& ItemId, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventResource(FlowType, Amount, *VirtualCurrency, *ItemCategory, *ItemId, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordResourceWithReasonWithAttrs(EsparklogsAnalyticsFlowType FlowType, float Amount, const FString& VirtualCurrency, const FString& ItemCategory, const FString& ItemId, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventResource(FlowType, Amount, *VirtualCurrency, *ItemCategory, *ItemId, *Reason, CustomAttrs);
}

bool UsparklogsAnalytics::AddProgression1(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression2(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, nullptr, nullptr, nullptr, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression3(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, P3, nullptr, nullptr, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression4(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, P3, P4, nullptr, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression5(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, P3, P4, P5, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression1(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, nullptr, nullptr, nullptr, nullptr, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression2(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, nullptr, nullptr, nullptr, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression3(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, P3, nullptr, nullptr, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression4(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, P3, P4, nullptr, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression5(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, P3, P4, P5, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression1(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, nullptr, nullptr, nullptr, nullptr, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression2(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, nullptr, nullptr, nullptr, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression3(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, P3, nullptr, nullptr, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression4(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, P3, P4, nullptr, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression5(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, P3, P4, P5, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression1(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, nullptr, nullptr, nullptr, nullptr, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression2(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, nullptr, nullptr, nullptr, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression3(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, P3, nullptr, nullptr, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression4(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, P3, P4, nullptr, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression5(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, P1, P2, P3, P4, P5, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression1(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression2(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, nullptr, nullptr, nullptr, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression3(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, nullptr, nullptr, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression4(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, P4, nullptr, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression5(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, P4, P5, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression1(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, nullptr, nullptr, nullptr, nullptr, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression2(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, nullptr, nullptr, nullptr, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression3(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, nullptr, nullptr, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression4(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, P4, nullptr, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression5(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, P4, P5, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression1(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, nullptr, nullptr, nullptr, nullptr, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression2(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, nullptr, nullptr, nullptr, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression3(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, nullptr, nullptr, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression4(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, P4, nullptr, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression5(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, P4, P5, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression1(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, nullptr, nullptr, nullptr, nullptr, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression2(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, nullptr, nullptr, nullptr, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression3(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, nullptr, nullptr, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression4(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, P4, nullptr, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgression5(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, P4, P5, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, nullptr, PArray, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, nullptr, PArray, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, nullptr, PArray, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, nullptr, PArray, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, double Value, const TArray<FString>& PArray, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, &Value, PArray, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, double Value, const TArray<FString>& PArray, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, &Value, PArray, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, double Value, const TArray<FString>& PArray, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, &Value, PArray, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, double Value, const TArray<FString>& PArray, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, &Value, PArray, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

void UsparklogsAnalytics::RecordProgression1(EsparklogsAnalyticsProgressionStatus Status, const FString& P1)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordProgression2(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, *P2, nullptr, nullptr, nullptr, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordProgression3(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, *P2, *P3, nullptr, nullptr, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordProgression4(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& P4)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, *P2, *P3, *P4, nullptr, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordProgression5(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& P5)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, *P2, *P3, *P4, *P5, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithReason1(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, nullptr, nullptr, nullptr, nullptr, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithReason2(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, *P2, nullptr, nullptr, nullptr, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithReason3(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, *P2, *P3, nullptr, nullptr, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithReason4(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, *P2, *P3, *P4, nullptr, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithReason5(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& P5, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, *P2, *P3, *P4, *P5, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithReasonWithAttrs1(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, nullptr, nullptr, nullptr, nullptr, *Reason, CustomAttrs);
}

void UsparklogsAnalytics::RecordProgressionWithReasonWithAttrs2(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, *P2, nullptr, nullptr, nullptr, *Reason, CustomAttrs);
}

void UsparklogsAnalytics::RecordProgressionWithReasonWithAttrs3(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, *P2, *P3, nullptr, nullptr, *Reason, CustomAttrs);
}

void UsparklogsAnalytics::RecordProgressionWithReasonWithAttrs4(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, *P2, *P3, *P4, nullptr, *Reason, CustomAttrs);
}

void UsparklogsAnalytics::RecordProgressionWithReasonWithAttrs5(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& P5, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, *P1, *P2, *P3, *P4, *P5, *Reason, CustomAttrs);
}

void UsparklogsAnalytics::RecordProgressionWithValue1(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithValue2(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, *P2, nullptr, nullptr, nullptr, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithValue3(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, *P2, *P3, nullptr, nullptr, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithValue4(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& P4)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, *P2, *P3, *P4, nullptr, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithValue5(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& P5)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, *P2, *P3, *P4, *P5, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithValueWithReason1(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, nullptr, nullptr, nullptr, nullptr, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithValueWithReason2(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, *P2, nullptr, nullptr, nullptr, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithValueWithReason3(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, *P2, *P3, nullptr, nullptr, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithValueWithReason4(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, *P2, *P3, *P4, nullptr, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithValueWithReason5(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& P5, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, *P2, *P3, *P4, *P5, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordProgressionWithValueWithReasonWithAttrs1(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, nullptr, nullptr, nullptr, nullptr, *Reason, CustomAttrs);
}

void UsparklogsAnalytics::RecordProgressionWithValueWithReasonWithAttrs2(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, *P2, nullptr, nullptr, nullptr, *Reason, CustomAttrs);
}

void UsparklogsAnalytics::RecordProgressionWithValueWithReasonWithAttrs3(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, *P2, *P3, nullptr, nullptr, *Reason, CustomAttrs);
}

void UsparklogsAnalytics::RecordProgressionWithValueWithReasonWithAttrs4(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, *P2, *P3, *P4, nullptr, *Reason, CustomAttrs);
}

void UsparklogsAnalytics::RecordProgressionWithValueWithReasonWithAttrs5(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& P5, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, Value, *P1, *P2, *P3, *P4, *P5, *Reason, CustomAttrs);
}

void UsparklogsAnalytics::RecordProgressionArray(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, nullptr, PArray, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordProgressionArrayWithAttrs(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		FsparklogsAnalyticsProvider::AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, nullptr, PArray, nullptr, CustomObject);
}

void UsparklogsAnalytics::RecordProgressionArrayWithReason(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, nullptr, PArray, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordProgressionArrayWithReasonWithAttrs(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		FsparklogsAnalyticsProvider::AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, nullptr, PArray, *Reason, CustomObject);
}

void UsparklogsAnalytics::RecordProgressionArrayWithValue(EsparklogsAnalyticsProgressionStatus Status, float Value, const TArray<FString>& PArray)
{
	double ValueDouble = Value;
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, &ValueDouble, PArray, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordProgressionArrayWithValueWithAttrs(EsparklogsAnalyticsProgressionStatus Status, float Value, const TArray<FString>& PArray, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		FsparklogsAnalyticsProvider::AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	double ValueDouble = Value;
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, &ValueDouble, PArray, nullptr, CustomObject);
}

void UsparklogsAnalytics::RecordProgressionArrayWithValueWithReason(EsparklogsAnalyticsProgressionStatus Status, float Value, const TArray<FString>& PArray, const FString& Reason)
{
	double ValueDouble = Value;
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, &ValueDouble, PArray, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordProgressionArrayWithValueWithReasonWithAttrs(EsparklogsAnalyticsProgressionStatus Status, float Value, const TArray<FString>& PArray, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		FsparklogsAnalyticsProvider::AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	double ValueDouble = Value;
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventProgression(Status, &ValueDouble, PArray, *Reason, CustomObject);
}

bool UsparklogsAnalytics::AddDesign(const TCHAR* EventId, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventId, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesign(const TCHAR* EventId, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventId, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesign(const TCHAR* EventId, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventId, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesign(const TCHAR* EventId, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventId, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesign(const TCHAR* EventId, double Value, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventId, Value, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesign(const TCHAR* EventId, double Value, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventId, Value, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesign(const TCHAR* EventId, double Value, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventId, Value, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesign(const TCHAR* EventId, double Value, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventId, Value, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesignArray(const TArray<FString>& EventIDParts, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, nullptr, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesignArray(const TArray<FString>& EventIDParts, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, nullptr, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesignArray(const TArray<FString>& EventIDParts, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, nullptr, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesignArray(const TArray<FString>& EventIDParts, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, nullptr, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesignArray(const TArray<FString>& EventIDParts, double Value, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, &Value, nullptr, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesignArray(const TArray<FString>& EventIDParts, double Value, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, &Value, nullptr, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesignArray(const TArray<FString>& EventIDParts, double Value, const TCHAR* Reason, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, &Value, Reason, nullptr, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool UsparklogsAnalytics::AddDesignArray(const TArray<FString>& EventIDParts, double Value, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, &Value, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

void UsparklogsAnalytics::RecordDesign(const FString& EventId)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(*EventId, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordDesignWithAttr(const FString& EventId, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(*EventId, nullptr, CustomAttrs);
}

void UsparklogsAnalytics::RecordDesignWithReason(const FString& EventId, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(*EventId, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordDesignWithReasonWithAttr(const FString& EventId, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(*EventId, *Reason, CustomAttrs);
}

void UsparklogsAnalytics::RecordDesignWithValue(const FString& EventId, float Value)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(*EventId, Value, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordDesignWithValueWithAttr(const FString& EventId, float Value, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(*EventId, Value, nullptr, CustomAttrs);
}

void UsparklogsAnalytics::RecordDesignWithValueWithReason(const FString& EventId, float Value, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(*EventId, Value, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordDesignWithValueWithReasonWithAttr(const FString& EventId, float Value, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(*EventId, Value, *Reason, CustomAttrs);
}

void UsparklogsAnalytics::RecordDesignArrayWithValue(const TArray<FString>& EventIDParts, float Value)
{
	double ValueDouble = Value;
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, &ValueDouble, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordDesignArrayWithValueWithAttr(const TArray<FString>& EventIDParts, float Value, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		FsparklogsAnalyticsProvider::AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	double ValueDouble = Value;
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, &ValueDouble, nullptr, CustomObject);
}

void UsparklogsAnalytics::RecordDesignArrayWithValueWithReason(const TArray<FString>& EventIDParts, float Value, const FString& Reason)
{
	double ValueDouble = Value;
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, &ValueDouble, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordDesignArrayWithValueWithReasonWithAttr(const TArray<FString>& EventIDParts, float Value, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		FsparklogsAnalyticsProvider::AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	double ValueDouble = Value;
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, &ValueDouble, *Reason, CustomObject);
}

void UsparklogsAnalytics::RecordDesignArray(const TArray<FString>& EventIDParts)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, nullptr, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordDesignArrayWithAttr(const TArray<FString>& EventIDParts, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		FsparklogsAnalyticsProvider::AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, nullptr, nullptr, CustomObject);
}

void UsparklogsAnalytics::RecordDesignArrayWithReason(const TArray<FString>& EventIDParts, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, nullptr, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordDesignArrayWithReasonWithAttr(const TArray<FString>& EventIDParts, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		FsparklogsAnalyticsProvider::AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventDesign(EventIDParts, nullptr, *Reason, CustomObject);
}

bool UsparklogsAnalytics::AddLog(EsparklogsSeverity Severity, const TCHAR* Message, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventLog(Severity, Message, nullptr, nullptr, OverrideSession);
}

bool UsparklogsAnalytics::AddLog(EsparklogsSeverity Severity, const TCHAR* Message, TSharedPtr<FJsonObject> CustomAttrs, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventLog(Severity, Message, nullptr, CustomAttrs, OverrideSession);
}

bool UsparklogsAnalytics::AddLog(EsparklogsSeverity Severity, const TCHAR* Message, const TCHAR* Reason, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventLog(Severity, Message, Reason, nullptr, OverrideSession);
}

bool UsparklogsAnalytics::AddLog(EsparklogsSeverity Severity, const TCHAR* Message, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	return FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventLog(Severity, Message, Reason, CustomAttrs, OverrideSession);
}

void UsparklogsAnalytics::RecordLog(EsparklogsSeverity Severity, const FString& Message)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventLog(Severity, *Message, nullptr, nullptr);
}

void UsparklogsAnalytics::RecordLogWithAttr(EsparklogsSeverity Severity, const FString& Message, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventLog(Severity, *Message, nullptr, CustomAttrs);
}

void UsparklogsAnalytics::RecordLogWithReason(EsparklogsSeverity Severity, const FString& Message, const FString& Reason)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventLog(Severity, *Message, *Reason, nullptr);
}

void UsparklogsAnalytics::RecordLogWithReasonWithAttr(EsparklogsSeverity Severity, const FString& Message, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs)
{
	FsparklogsModule::GetAnalyticsProvider()->CreateAnalyticsEventLog(Severity, *Message, *Reason, CustomAttrs);
}

// =============== FsparklogsAnalyticsProvider ===============================================================================

const TCHAR* const FsparklogsAnalyticsProvider::RecordProgressDelimiters[2] = { TEXT(":"), TEXT(".") };

FsparklogsAnalyticsProvider::FsparklogsAnalyticsProvider(TSharedRef<FsparklogsSettings> InSettings)
	: Settings(InSettings)
	, SessionStarted(ITLEmptyDateTime)
	, SessionNumber(0)
	, MetaAttributes(new FJsonObject())
{
	SetupDefaultMetaAttributes();
}

FsparklogsAnalyticsProvider::~FsparklogsAnalyticsProvider()
{
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventPurchase(const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* RealCurrencyCode, double Amount, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	if (!FsparklogsModule::IsModuleLoaded())
	{
		return false;
	}
	if (!AutoStartSessionBeforeEvent())
	{
		return false;
	}
	TSharedPtr<FJsonObject> Data(new FJsonObject());
	FString EventID;
	TArray<TSharedPtr<FJsonValue>> EventIDParts;
	if (ItemCategory != nullptr && *ItemCategory != 0)
	{
		EventID = ItemCategory;
		Data->SetStringField(PurchaseFieldItemCategory, ItemCategory);
		EventIDParts.Add(TSharedPtr<FJsonValue>(new FJsonValueString(ItemCategory)));
	}
	else
	{
		ItemCategory = TEXT("");
	}
	if (ItemId != nullptr && *ItemId != 0)
	{
		if (!EventID.IsEmpty())
		{
			EventID += ItemSeparator;
		}
		EventID += ItemId;
		Data->SetStringField(PurchaseFieldItemId, ItemId);
		EventIDParts.Add(TSharedPtr<FJsonValue>(new FJsonValueString(ItemId)));
	}
	else
	{
		ItemId = TEXT("");
	}
	if (!EventID.IsEmpty())
	{
		Data->SetStringField(PurchaseFieldEventId, EventID);
		Data->SetArrayField(PurchaseFieldEventIdParts, EventIDParts);
	}
	if (RealCurrencyCode == nullptr || *RealCurrencyCode == 0)
	{
		RealCurrencyCode = TEXT("USD");
	}
	FString RealCurrencyCodeStr(RealCurrencyCode);
	RealCurrencyCodeStr.ToUpperInline();
	Data->SetStringField(PurchaseFieldCurrency, RealCurrencyCodeStr);
	Data->SetNumberField(PurchaseFieldAmount, Amount);
	if (Reason != nullptr && *Reason != 0)
	{
		Data->SetStringField(PurchaseFieldReason, Reason);
	}
	else
	{
		Reason = TEXT("");
	}
	if (CustomAttrs.IsValid() && CustomAttrs->Values.Num() > 0)
	{
		Data->SetObjectField(StandardFieldCustom, CustomAttrs);
	}
	FinalizeAnalyticsEvent(EventTypePurchase, OverrideSession, Data);
	FString DefaultMessage = FString::Printf(TEXT("%s: %s: purchase of item made; item_category=`%s` item_id=`%s` currency=`%s` amount=%.2f reason=`%s`"), MessageHeader, EventTypePurchase, ItemCategory, ItemId, RealCurrencyCode, Amount, Reason);
	return FsparklogsModule::GetModule().AddRawAnalyticsEvent(Data, *CalculateFinalMessage(DefaultMessage, IncludeDefaultMessage, ExtraMessage), nullptr, IncludeDefaultMessage, false);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventPurchase(const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* RealCurrencyCode, double Amount, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	return CreateAnalyticsEventPurchase(ItemCategory, ItemId, RealCurrencyCode, Amount, Reason, CustomObject, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventResource(EsparklogsAnalyticsFlowType FlowType, double Amount, const TCHAR* VirtualCurrency, const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	if (!FsparklogsModule::IsModuleLoaded() || VirtualCurrency == nullptr || *VirtualCurrency == 0)
	{
		return false;
	}
	if (!AutoStartSessionBeforeEvent())
	{
		return false;
	}

	double AmountAbs = FMath::Abs(Amount);
	if (FlowType == EsparklogsAnalyticsFlowType::Source)
	{
		Amount = AmountAbs;
	}
	else
	{
		Amount = -AmountAbs;
	}

	TSharedPtr<FJsonObject> Data(new FJsonObject());
	FString FlowTypeStr = UEnum::GetDisplayValueAsText(FlowType).ToString();
	Data->SetStringField(ResourceFieldFlowType, FlowTypeStr);
	Data->SetStringField(ResourceFieldVirtualCurrency, VirtualCurrency);
	FString EventID = (FlowTypeStr + ItemSeparator) + VirtualCurrency;
	TArray<TSharedPtr<FJsonValue>> EventIDParts;
	EventIDParts.Add(TSharedPtr<FJsonValue>(new FJsonValueString(FlowTypeStr)));
	EventIDParts.Add(TSharedPtr<FJsonValue>(new FJsonValueString(VirtualCurrency)));
	if (ItemCategory != nullptr && *ItemCategory != 0)
	{
		EventID += ItemSeparator;
		EventID += ItemCategory;
		Data->SetStringField(ResourceFieldItemCategory, ItemCategory);
		EventIDParts.Add(TSharedPtr<FJsonValue>(new FJsonValueString(ItemCategory)));
	}
	else
	{
		ItemCategory = TEXT("");
	}
	if (ItemId != nullptr && *ItemId != 0)
	{
		EventID += ItemSeparator;
		EventID += ItemId;
		Data->SetStringField(ResourceFieldItemId, ItemId);
		EventIDParts.Add(TSharedPtr<FJsonValue>(new FJsonValueString(ItemId)));
	}
	else
	{
		ItemId = TEXT("");
	}
	Data->SetStringField(ResourceFieldEventId, EventID);
	Data->SetArrayField(ResourceFieldEventIdParts, EventIDParts);
	Data->SetNumberField(ResourceFieldAmount, Amount);
	if (Reason != nullptr && *Reason != 0)
	{
		Data->SetStringField(ResourceFieldReason, Reason);
	}
	else
	{
		Reason = TEXT("");
	}
	if (CustomAttrs.IsValid() && CustomAttrs->Values.Num() > 0)
	{
		Data->SetObjectField(StandardFieldCustom, CustomAttrs);
	}
	FinalizeAnalyticsEvent(EventTypeResource, OverrideSession, Data);
	FString DefaultMessage = FString::Printf(TEXT("%s: %s: flow_type=%s virtual_currency=`%s` item_category=`%s` item_id=`%s` amount=%f reason=`%s`"), MessageHeader, EventTypeResource, *FlowTypeStr, VirtualCurrency, ItemCategory, ItemId, Amount, Reason);
	return FsparklogsModule::GetModule().AddRawAnalyticsEvent(Data, *CalculateFinalMessage(DefaultMessage, IncludeDefaultMessage, ExtraMessage), nullptr, IncludeDefaultMessage, false);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventResource(EsparklogsAnalyticsFlowType FlowType, double Amount, const TCHAR* VirtualCurrency, const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	return CreateAnalyticsEventResource(FlowType, Amount, VirtualCurrency, ItemCategory, ItemId, Reason, CustomObject, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool FsparklogsAnalyticsProvider::ValidateProgressionEvent(const TArray<FString>& PArray)
{
	bool PrevIsEmpty = false;
	if (PArray.Num() <= 0)
	{
		return false;
	}
	for (auto it = PArray.CreateConstIterator(); it; ++it)
	{
		const FString& S = *it;
		if (S.IsEmpty())
		{
			PrevIsEmpty = true;
		}
		else if (PrevIsEmpty)
		{
			return false;
		}
	}
	return true;
}

FString FsparklogsAnalyticsProvider::GetProgressionEventID(const TArray<FString>& PArray)
{
	if (!ValidateProgressionEvent(PArray))
	{
		return FString();
	}
	FString EventID;
	for (auto it = PArray.CreateConstIterator(); it; ++it)
	{
		const FString& S = *it;
		if (!S.IsEmpty())
		{
			if (!EventID.IsEmpty())
			{
				EventID += ItemSeparator;
			}
			EventID += S;
		}
	}
	return EventID;
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventProgression(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	TArray<FString> PArray;
	PArray.Add(P1 == nullptr ? TEXT("") : FString(P1));
	PArray.Add(P2 == nullptr ? TEXT("") : FString(P2));
	PArray.Add(P3 == nullptr ? TEXT("") : FString(P3));
	PArray.Add(P4 == nullptr ? TEXT("") : FString(P4));
	PArray.Add(P5 == nullptr ? TEXT("") : FString(P5));
	return CreateAnalyticsEventProgression(Status, &Value, PArray, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventProgression(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	return CreateAnalyticsEventProgression(Status, Value, P1, P2, P3, P4, P5, Reason, CustomObject, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventProgression(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	TArray<FString> PArray;
	PArray.Add(P1 == nullptr ? TEXT("") : FString(P1));
	PArray.Add(P2 == nullptr ? TEXT("") : FString(P2));
	PArray.Add(P3 == nullptr ? TEXT("") : FString(P3));
	PArray.Add(P4 == nullptr ? TEXT("") : FString(P4));
	PArray.Add(P5 == nullptr ? TEXT("") : FString(P5));
	return CreateAnalyticsEventProgression(Status, nullptr, PArray, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventProgression(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	return CreateAnalyticsEventProgression(Status, P1, P2, P3, P4, P5, Reason, CustomObject, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventProgression(EsparklogsAnalyticsProgressionStatus Status, double* Value, const TArray<FString>& PArray, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	FString Tiers = GetProgressionEventID(PArray);
	if (!FsparklogsModule::IsModuleLoaded() || Tiers.IsEmpty())
	{
		return false;
	}
	if (!AutoStartSessionBeforeEvent())
	{
		return false;
	}

	bool IncrementAttempt = (Status == EsparklogsAnalyticsProgressionStatus::Started);
	FString AttemptID = (FString(EventTypeProgression) + ItemSeparator + Tiers).ToLower();
	FScopeLock WriteLock(&DataCriticalSection);
	bool AttemptWasInProgress = InProgressProgression.Contains(AttemptID);
	if (Status == EsparklogsAnalyticsProgressionStatus::Started)
	{
		InProgressProgression.Add(AttemptID);
	}
	else
	{
		InProgressProgression.Remove(AttemptID);
	}
	WriteLock.Unlock();
	if (Status == EsparklogsAnalyticsProgressionStatus::Started && AttemptWasInProgress)
	{
		// This particular progression event was already in progress and now we're trying to start again, so mark the previous one as failed first.
		CreateAnalyticsEventProgression(EsparklogsAnalyticsProgressionStatus::Failed, nullptr, PArray, TEXT("starting new attempt before finishing the previous attempt"), CustomAttrs, true, nullptr, OverrideSession);
	}
	else if (Status != EsparklogsAnalyticsProgressionStatus::Started && !AttemptWasInProgress)
	{
		// There was no previous start event active, so we need to increment the attempt number for this new attempt (that is now finishing)
		IncrementAttempt = true;
	}
	int AttemptNumber = Settings->GetAttemptNumber(AttemptID, IncrementAttempt, Status == EsparklogsAnalyticsProgressionStatus::Completed);

	FString StatusStr = UEnum::GetDisplayValueAsText(Status).ToString();
	FString EventId = StatusStr + ItemSeparator + Tiers;
	TArray<TSharedPtr<FJsonValue>> EventIdParts;
	EventIdParts.Add(TSharedPtr<FJsonValue>(new FJsonValueString(StatusStr)));
	for (int i = 0; i < PArray.Num(); i++)
	{
		EventIdParts.Add(TSharedPtr<FJsonValue>(new FJsonValueString(PArray[i])));
	}
	TSharedPtr<FJsonObject> Data(new FJsonObject());
	Data->SetStringField(DesignFieldEventId, EventId);
	Data->SetArrayField(DesignFieldEventIdParts, EventIdParts);
	Data->SetStringField(ProgressionFieldStatus, StatusStr);
	Data->SetStringField(ProgressionFieldTiersString, Tiers);
	Data->SetField(ProgressionFieldTiersArray, ConvertNonEmptyStringArrayToJSON(PArray));
	for (int i=0; i<PArray.Num(); i++)
	{
		const FString& P = PArray[i];
		if (!P.IsEmpty())
		{
			Data->SetStringField(FString::Printf(TEXT("%s%d"), ProgressionFieldTierPrefix, i+1), P);
		}
	}
	if (Value != nullptr)
	{
		Data->SetNumberField(ProgressionFieldValue, *Value);
	}
	Data->SetNumberField(ProgressionFieldAttempt, (double)AttemptNumber);
	if (Reason != nullptr && *Reason != 0)
	{
		Data->SetStringField(ProgressionFieldReason, FString(Reason));
	}
	else
	{
		Reason = TEXT("");
	}
	if (CustomAttrs.IsValid() && CustomAttrs->Values.Num() > 0)
	{
		Data->SetObjectField(StandardFieldCustom, CustomAttrs);
	}
	FinalizeAnalyticsEvent(EventTypeProgression, OverrideSession, Data);
	FString ValueDesc = Value == nullptr ? FString(TEXT("null")) : FString::Printf(TEXT("%f"), *Value);
	FString DefaultMessage = FString::Printf(TEXT("%s: %s: event_id=`%s` value=%s reason=`%s`"), MessageHeader, EventTypeProgression, *EventId, *ValueDesc, Reason);
	return FsparklogsModule::GetModule().AddRawAnalyticsEvent(Data, *CalculateFinalMessage(DefaultMessage, IncludeDefaultMessage, ExtraMessage), nullptr, IncludeDefaultMessage, false);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventDesign(const TCHAR* EventId, double Value, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	if (!FsparklogsModule::IsModuleLoaded() || EventId == nullptr)
	{
		return false;
	}
	FString EventIdStr(EventId);
	TArray<FString> EventIdParts;
	EventIdStr.ParseIntoArray(EventIdParts, ItemSeparator, false);
	return CreateAnalyticsEventDesign(EventIdParts, &Value, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventDesign(const TCHAR* EventId, double Value, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	return CreateAnalyticsEventDesign(EventId, Value, Reason, CustomObject, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventDesign(const TCHAR* EventId, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	if (!FsparklogsModule::IsModuleLoaded() || EventId == nullptr)
	{
		return false;
	}
	FString EventIdStr(EventId);
	TArray<FString> EventIdParts;
	EventIdStr.ParseIntoArray(EventIdParts, ItemSeparator, false);
	return CreateAnalyticsEventDesign(EventIdParts, nullptr, Reason, CustomAttrs, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventDesign(const TCHAR* EventId, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	return CreateAnalyticsEventDesign(EventId, Reason, CustomObject, IncludeDefaultMessage, ExtraMessage, OverrideSession);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventDesign(const TArray<FString>& EventIDParts, double* Value, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage, const TCHAR* ExtraMessage, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	FString EventId = GetProgressionEventID(EventIDParts);
	if (!FsparklogsModule::IsModuleLoaded() || EventId.IsEmpty())
	{
		return false;
	}
	if (!AutoStartSessionBeforeEvent())
	{
		return false;
	}
	TSharedPtr<FJsonObject> Data(new FJsonObject());
	Data->SetStringField(DesignFieldEventId, EventId);
	TArray<TSharedPtr<FJsonValue>> EventIdPartsJson;
	for (const FString& Part : EventIDParts)
	{
		EventIdPartsJson.Add(TSharedPtr<FJsonValue>(new FJsonValueString(Part)));
	}
	Data->SetArrayField(DesignFieldEventIdParts, EventIdPartsJson);
	if (Value != nullptr)
	{
		Data->SetNumberField(DesignFieldValue, *Value);
	}
	if (Reason != nullptr && *Reason != 0)
	{
		Data->SetStringField(DesignFieldReason, Reason);
	}
	else
	{
		Reason = TEXT("");
	}
	if (CustomAttrs.IsValid() && CustomAttrs->Values.Num() > 0)
	{
		Data->SetObjectField(StandardFieldCustom, CustomAttrs);
	}
	FinalizeAnalyticsEvent(EventTypeDesign, OverrideSession, Data);
	FString ValueDesc = Value == nullptr ? TEXT("null") : FString::Printf(TEXT("%f"), *Value);
	FString DefaultMessage = FString::Printf(TEXT("%s: %s: event_id=`%s` value=%s reason=`%s`"), MessageHeader, EventTypeDesign, *EventId, *ValueDesc, Reason);
	return FsparklogsModule::GetModule().AddRawAnalyticsEvent(Data, *CalculateFinalMessage(DefaultMessage, IncludeDefaultMessage, ExtraMessage), nullptr, IncludeDefaultMessage, false);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventLog(EsparklogsSeverity Severity, const TCHAR* Message, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	if (!FsparklogsModule::IsModuleLoaded() || Message == nullptr)
	{
		return false;
	}
	if (!AutoStartSessionBeforeEvent())
	{
		return false;
	}
	TSharedPtr<FJsonObject> RootData(new FJsonObject());
	RootData->SetStringField(LogFieldSeverity, UEnum::GetDisplayValueAsText(Severity).ToString());
	TSharedPtr<FJsonObject> Data(new FJsonObject());
	if (Reason != nullptr && *Reason != 0)
	{
		Data->SetStringField(LogFieldReason, Reason);
	}
	if (CustomAttrs.IsValid() && CustomAttrs->Values.Num() > 0)
	{
		Data->SetObjectField(StandardFieldCustom, CustomAttrs);
	}
	FinalizeAnalyticsEvent(EventTypeLog, OverrideSession, Data);
	return FsparklogsModule::GetModule().AddRawAnalyticsEvent(Data, Message, RootData, false, false);
}

bool FsparklogsAnalyticsProvider::CreateAnalyticsEventLog(EsparklogsSeverity Severity, const TCHAR* Message, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (CustomAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		AddAnalyticsEventAttributesToJsonObject(CustomObject, CustomAttrs);
	}
	return CreateAnalyticsEventLog(Severity, Message, Reason, CustomObject, OverrideSession);
}

bool FsparklogsAnalyticsProvider::StartSession(const TArray<FAnalyticsEventAttribute>& Attributes)
{
	return StartSession(nullptr, Attributes);
}

bool FsparklogsAnalyticsProvider::StartSession(const TCHAR* Reason, const TArray<FAnalyticsEventAttribute>& Attributes)
{
	if (!FsparklogsModule::IsModuleLoaded())
	{
		return false;
	}
	FScopeLock WriteLock(&DataCriticalSection);
	if (!CurrentSessionID.IsEmpty())
	{
		// Session already started, treat as success
		return true;
	}
	CurrentSessionID = ITLGenerateNewRandomID();
	SessionStarted = FDateTime::UtcNow();
	SessionNumber = Settings->GetSessionNumber(true);
	TSharedPtr<FJsonObject> Data(new FJsonObject());
	if (Reason != nullptr)
	{
		Data->SetStringField(SessionStartFieldReason, Reason);
	}
	InternalFinalizeAnalyticsEvent(EventTypeSessionStart, nullptr, Data);
	WriteLock.Unlock();
	Settings->MarkStartOfAnalyticsSession(CurrentSessionID, SessionStarted);
	return FsparklogsModule::GetModule().AddRawAnalyticsEvent(Data, *FString::Printf(TEXT("%s: %s: started new session"), MessageHeader, EventTypeSessionStart), nullptr, false, true);
}

void FsparklogsAnalyticsProvider::EndSession()
{
	DoEndSession(nullptr, FDateTime::UtcNow());
}

void FsparklogsAnalyticsProvider::EndSession(const TCHAR* Reason)
{
	DoEndSession(Reason, FDateTime::UtcNow());
}

void FsparklogsAnalyticsProvider::DoEndSession(const TCHAR* Reason, FDateTime SessionEnded)
{
	if (!FsparklogsModule::IsModuleLoaded())
	{
		return;
	}
	FScopeLock WriteLock(&DataCriticalSection);
	if (CurrentSessionID.IsEmpty())
	{
		return;
	}

	TSharedPtr<FJsonObject> Data(new FJsonObject());
	Data->SetStringField(SessionEndFieldSessionEnded, ITLGetUTCDateTimeAsRFC3339(SessionEnded));
	FTimespan SessionDuration = SessionEnded - SessionStarted;
	double SessionDurationSecs = SessionDuration.GetTotalSeconds();
	if (SessionDurationSecs > 0.0 || SessionDurationSecs < (60.0 * 60.0 * 24 * 30 * 12))
	{
		Data->SetNumberField(SessionEndFieldSessionDurationSecs, SessionDurationSecs);
	}
	if (Reason != nullptr)
	{
		Data->SetStringField(SessionEndFieldReason, Reason);
	}
	InternalFinalizeAnalyticsEvent(EventTypeSessionEnd, nullptr, Data);

	// Now reset the session data, release write lock, and send the data
	CurrentSessionID.Reset();
	SessionStarted = ITLEmptyDateTime;
	SessionNumber = 0;
	WriteLock.Unlock();
	FsparklogsModule::GetModule().AddRawAnalyticsEvent(Data, *FString::Printf(TEXT("%s: %s: finished session normally"), MessageHeader, EventTypeSessionEnd), nullptr, false, true);
	// The app might end or go inactive soon, try to get the data to the cloud asap...
	FsparklogsModule::GetModule().Flush();
	Settings->MarkEndOfAnalyticsSession();
}

FString FsparklogsAnalyticsProvider::GetSessionID() const
{
	FScopeLock ReadLock(&DataCriticalSection);
	return CurrentSessionID;
}

FSparkLogsAnalyticsSessionDescriptor FsparklogsAnalyticsProvider::GetSessionDescriptor()
{
	FScopeLock ReadLock(&DataCriticalSection);
	if (CurrentSessionID.IsEmpty())
	{
		return FSparkLogsAnalyticsSessionDescriptor();
	}
	else
	{
		return FSparkLogsAnalyticsSessionDescriptor(*CurrentSessionID, SessionNumber, SessionStarted, *Settings->GetEffectiveAnalyticsUserID());
	}
}

bool FsparklogsAnalyticsProvider::SetSessionID(const FString& InSessionID)
{
	FString CurID = GetSessionID();
	if (CurID == InSessionID)
	{
		// no-op
		return true;
	}
	AutoCleanupSession();
	FScopeLock WriteLock(&DataCriticalSection);
	CurrentSessionID = InSessionID;
	return true;
}

void FsparklogsAnalyticsProvider::FlushEvents()
{
	if (!FsparklogsModule::IsModuleLoaded())
	{
		return;
	}
	FsparklogsModule::GetModule().Flush();
}

void FsparklogsAnalyticsProvider::SetUserID(const FString& InUserID)
{
	FString CurrentID = Settings->GetEffectiveAnalyticsUserID();
	if (CurrentID == InUserID)
	{
		// no-op
		return;
	}
	AutoCleanupSession();
	return Settings->SetUserID(*InUserID);
}

FString FsparklogsAnalyticsProvider::GetUserID() const
{
	return Settings->GetEffectiveAnalyticsUserID();
}

void FsparklogsAnalyticsProvider::SetBuildInfo(const FString& InBuildInfo)
{
	FAnalyticsEventAttribute Attr(MetaFieldBuild, InBuildInfo);
	FScopeLock WriteLock(&DataCriticalSection);
	AddAnalyticsEventAttributeToJsonObject(MetaAttributes, Attr, 1);
}

void FsparklogsAnalyticsProvider::SetGender(const FString& InGender)
{
	FAnalyticsEventAttribute Attr(MetaFieldGender, InGender);
	FScopeLock WriteLock(&DataCriticalSection);
	AddAnalyticsEventAttributeToJsonObject(MetaAttributes, Attr, 1);
}

void FsparklogsAnalyticsProvider::SetLocation(const FString& InLocation)
{
	FAnalyticsEventAttribute Attr(MetaFieldLocation, InLocation);
	FScopeLock WriteLock(&DataCriticalSection);
	AddAnalyticsEventAttributeToJsonObject(MetaAttributes, Attr, 1);
}

void FsparklogsAnalyticsProvider::SetAge(const int32 InAge)
{
	FAnalyticsEventAttribute Attr(MetaFieldAge, InAge);
	FScopeLock WriteLock(&DataCriticalSection);
	AddAnalyticsEventAttributeToJsonObject(MetaAttributes, Attr, 1);
}

void FsparklogsAnalyticsProvider::RecordEvent(const FString& EventName, const TArray<FAnalyticsEventAttribute>& Attributes)
{
	FString EffectiveEventId = EventName;
	if (EventName.IsEmpty())
	{
		if (Attributes.Num() <= 0)
		{
			return;
		}
		EffectiveEventId = RecordEventGenericEventId;
	}
	TSharedPtr<FJsonObject> CustomObject;
	if (Attributes.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		AddAnalyticsEventAttributesToJsonObject(CustomObject, Attributes);
	}
	CreateAnalyticsEventDesign(*EffectiveEventId, nullptr, CustomObject);
}

void FsparklogsAnalyticsProvider::RecordItemPurchase(const FString& ItemId, const FString& Currency, int PerItemCost, int ItemQuantity)
{
	TSharedPtr<FJsonObject> CustomObject(new FJsonObject());
	CustomObject->SetNumberField(RecordItemPurchasePerItemCost, (double)PerItemCost);
	CustomObject->SetNumberField(RecordItemPurchaseItemQuantity, (double)ItemQuantity);
	CreateAnalyticsEventResource(EsparklogsAnalyticsFlowType::Sink, PerItemCost * ItemQuantity, *Currency, RecordItemPurchaseItemCategory, *ItemId, nullptr, nullptr);
}

void FsparklogsAnalyticsProvider::RecordItemPurchase(const FString& ItemId, int ItemQuantity, const TArray<FAnalyticsEventAttribute>& EventAttrs)
{
	FString VirtualCurrency;
	FString ItemCategory;
	double Amount = (double)ItemQuantity;
	for (const FAnalyticsEventAttribute& A : EventAttrs)
	{
		if (0 == A.GetName().Compare(TEXT("itemcategory"), ESearchCase::IgnoreCase)
			|| 0 == A.GetName().Compare(TEXT("item_category"), ESearchCase::IgnoreCase)
			|| 0 == A.GetName().Compare(TEXT("itemtype"), ESearchCase::IgnoreCase)
			|| 0 == A.GetName().Compare(TEXT("item_type"), ESearchCase::IgnoreCase))
		{
			ItemCategory = A.GetValue();
		}
		else if (0 == A.GetName().Compare(TEXT("currency"), ESearchCase::IgnoreCase)
			|| 0 == A.GetName().Compare(TEXT("virtual_currency"), ESearchCase::IgnoreCase)
			|| 0 == A.GetName().Compare(TEXT("game_currency"), ESearchCase::IgnoreCase))
		{
			VirtualCurrency = A.GetValue();
		}
		else if (0 == A.GetName().Compare(TEXT("amount"), ESearchCase::IgnoreCase)
			|| 0 == A.GetName().Compare(TEXT("cost"), ESearchCase::IgnoreCase))
		{
			Amount = FCString::Atod(*A.GetValue());
		}
	}
	TSharedPtr<FJsonObject> CustomObject(new FJsonObject());
	CustomObject->SetNumberField(RecordItemPurchaseItemQuantity, (double)ItemQuantity);
	AddAnalyticsEventAttributesToJsonObject(CustomObject, EventAttrs);
	if (VirtualCurrency.IsEmpty())
	{
		FString PurchasePrefix = FString(RecordCurrencyPurchasePurchaseItemCategory) + ItemSeparator;
		CreateAnalyticsEventDesign(*(PurchasePrefix + ItemId), Amount, nullptr, CustomObject);
	}
	else
	{
		CreateAnalyticsEventResource(EsparklogsAnalyticsFlowType::Sink, Amount, *VirtualCurrency, RecordItemPurchaseItemCategory, *ItemId, nullptr, nullptr);
	}
}

void FsparklogsAnalyticsProvider::RecordCurrencyPurchase(const FString& GameCurrencyType, int GameCurrencyAmount, const FString& RealCurrencyType, float RealMoneyCost, const FString& PaymentProvider)
{
	TSharedPtr<FJsonObject> CustomObject(new FJsonObject());
	CustomObject->SetStringField(RecordCurrencyPurchasePaymentProvider, PaymentProvider);
	CreateAnalyticsEventPurchase(RecordCurrencyPurchasePurchaseItemCategory, *GameCurrencyType, *RealCurrencyType, (double)RealMoneyCost, nullptr, CustomObject);
	CreateAnalyticsEventResource(EsparklogsAnalyticsFlowType::Source, (double)GameCurrencyAmount, *GameCurrencyType, RecordCurrencyPurchaseResourceItemCategory, nullptr, nullptr, CustomObject);
}

void FsparklogsAnalyticsProvider::RecordCurrencyPurchase(const FString& GameCurrencyType, int GameCurrencyAmount, const TArray<FAnalyticsEventAttribute>& EventAttrs)
{
	FString RealCurrency;
	bool FoundCost = false;
	double Cost=0.0;
	TSharedPtr<FJsonObject> CustomObject;
	if (EventAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		AddAnalyticsEventAttributesToJsonObject(CustomObject, EventAttrs);
	}
	for (const FAnalyticsEventAttribute& A : EventAttrs)
	{
		if (0 == A.GetName().Compare(TEXT("currency"), ESearchCase::IgnoreCase)
			|| 0 == A.GetName().Compare(TEXT("real_currency"), ESearchCase::IgnoreCase))
		{
			RealCurrency = A.GetValue();
		}
		else if (0 == A.GetName().Compare(TEXT("cost"), ESearchCase::IgnoreCase)
			|| 0 == A.GetName().Compare(TEXT("amount"), ESearchCase::IgnoreCase))
		{
			Cost = FCString::Atod(*A.GetValue());
			FoundCost = true;
		}
	}
	if (!RealCurrency.IsEmpty() && FoundCost)
	{
		CreateAnalyticsEventPurchase(RecordCurrencyPurchasePurchaseItemCategory, *GameCurrencyType, *RealCurrency, (double)Cost, nullptr, CustomObject);
	}
	CreateAnalyticsEventResource(EsparklogsAnalyticsFlowType::Source, (double)GameCurrencyAmount, *GameCurrencyType, RecordCurrencyPurchaseResourceItemCategory, nullptr, nullptr, CustomObject);
}

void FsparklogsAnalyticsProvider::RecordCurrencyGiven(const FString& GameCurrencyType, int GameCurrencyAmount, const TArray<FAnalyticsEventAttribute>& EventAttrs)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (EventAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		AddAnalyticsEventAttributesToJsonObject(CustomObject, EventAttrs);
	}
	CreateAnalyticsEventResource(EsparklogsAnalyticsFlowType::Source, (double)GameCurrencyAmount, *GameCurrencyType, RecordCurrencyGivenItemCategory, nullptr, nullptr, CustomObject);
}

void FsparklogsAnalyticsProvider::RecordError(const FString& Error, const TArray<FAnalyticsEventAttribute>& EventAttrs)
{
	TSharedPtr<FJsonObject> CustomObject;
	if (EventAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		AddAnalyticsEventAttributesToJsonObject(CustomObject, EventAttrs);
	}
	CreateAnalyticsEventLog(EsparklogsSeverity::Error, *Error, nullptr, CustomObject);
}

void FsparklogsAnalyticsProvider::RecordProgress(const FString& ProgressType, const FString& ProgressHierarchy)
{
	RecordProgress(ProgressType, ProgressHierarchy, TArray<FAnalyticsEventAttribute>());
}

void FsparklogsAnalyticsProvider::RecordProgress(const FString& ProgressType, const FString& ProgressHierarchy, const TArray<FAnalyticsEventAttribute>& EventAttrs)
{
	TArray<FString> PArray;
	ProgressHierarchy.ParseIntoArray(PArray, RecordProgressDelimiters, sizeof(RecordProgressDelimiters) / sizeof(RecordProgressDelimiters[0]), true);
	RecordProgress(ProgressType, PArray, EventAttrs);
}

void FsparklogsAnalyticsProvider::RecordProgress(const FString& ProgressType, const TArray<FString>& ProgressHierarchy, const TArray<FAnalyticsEventAttribute>& EventAttrs)
{
	EsparklogsAnalyticsProgressionStatus InvalidStatus = (EsparklogsAnalyticsProgressionStatus)INDEX_NONE;
	EsparklogsAnalyticsProgressionStatus Status = InvalidStatus;
	const UEnum* StatusEnum = StaticEnum<EsparklogsAnalyticsProgressionStatus>();
	if (ensure(StatusEnum))
	{
		Status = (EsparklogsAnalyticsProgressionStatus)StatusEnum->GetValueByName(FName(ProgressType));
	}
	if (Status == InvalidStatus)
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("FsparklogsAnalyticsProvider::RecordProgress ignoring unknown ProgressType=%s"), *ProgressType);
		return;
	}
	if (ProgressHierarchy.Num() <= 0)
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("FsparklogsAnalyticsProvider::RecordProgress ignoring empty ProgressHierarchy"));
		return;
	}
	double* ValuePtr = nullptr;
	double Value = 0.0;
	for (const FAnalyticsEventAttribute& A : EventAttrs)
	{
		if (0 == A.GetName().Compare(TEXT("value"), ESearchCase::IgnoreCase)
			|| 0 == A.GetName().Compare(TEXT("amount"), ESearchCase::IgnoreCase)
			|| 0 == A.GetName().Compare(TEXT("score"), ESearchCase::IgnoreCase))
		{
			Value = FCString::Atod(*A.GetValue());
			ValuePtr = &Value;
		}
	}
	TSharedPtr<FJsonObject> CustomObject;
	if (EventAttrs.Num() > 0)
	{
		CustomObject = TSharedPtr<FJsonObject>(new FJsonObject());
		AddAnalyticsEventAttributesToJsonObject(CustomObject, EventAttrs);
	}
	CreateAnalyticsEventProgression(Status, ValuePtr, ProgressHierarchy, nullptr, CustomObject);
}

FAnalyticsEventAttribute FsparklogsAnalyticsProvider::GetDefaultEventAttribute(int AttributeIndex) const
{
	FScopeLock ReadLock(&DataCriticalSection);
	if (AttributeIndex < 0 || AttributeIndex >= MetaAttributes->Values.Num())
	{
		return FAnalyticsEventAttribute();
	}
	auto it = MetaAttributes->Values.CreateConstIterator();
	for (int i = 0; i < AttributeIndex && it; i++)
	{
		++it;
	}
	if (!it || !it->Value.IsValid())
	{
		return FAnalyticsEventAttribute();
	}
	return ConvertJsonValueToEventAttribute(it->Key, it->Value);
}

FAnalyticsEventAttribute FsparklogsAnalyticsProvider::ConvertJsonValueToEventAttribute(const FString& Key, TSharedPtr<FJsonValue> Value)
{
	if (!Value.IsValid())
	{
		return FAnalyticsEventAttribute();
	}
	// ignore failure if they are trying to use this to get a complex JSON value
	FString ValueString;
	Value->TryGetString(ValueString);
	return FAnalyticsEventAttribute(Key, ValueString);
}

TSharedRef<FJsonValueArray> FsparklogsAnalyticsProvider::ConvertNonEmptyStringArrayToJSON(const TArray<FString>& A)
{
	TArray<TSharedPtr<FJsonValue>> InnerA;
	InnerA.Reserve(A.Num()+1);
	for (auto it = A.CreateConstIterator(); it; ++it)
	{
		const FString& S = *it;
		if (S.IsEmpty())
		{
			continue;
		}
		InnerA.Add(TSharedPtr<FJsonValue>(new FJsonValueString(S)));
	}
	TSharedRef<FJsonValueArray> JA(new FJsonValueArray(InnerA));
	return JA;
}

int32 FsparklogsAnalyticsProvider::GetDefaultEventAttributeCount() const
{
	FScopeLock ReadLock(&DataCriticalSection);
	return MetaAttributes->Values.Num();
}

TArray<FAnalyticsEventAttribute> FsparklogsAnalyticsProvider::GetDefaultEventAttributesSafe() const
{
	FScopeLock ReadLock(&DataCriticalSection);
	TArray<FAnalyticsEventAttribute> Ret;
	Ret.Reserve(MetaAttributes->Values.Num());
	for (auto it = MetaAttributes->Values.CreateConstIterator(); it; ++it)
	{
		auto Attr = ConvertJsonValueToEventAttribute(it->Key, it->Value);
		if (Attr.GetName().IsEmpty())
		{
			continue;
		}
		Ret.Add(Attr);
	}
	return Ret;
}

void FsparklogsAnalyticsProvider::SetDefaultEventAttributes(TArray<FAnalyticsEventAttribute>&& Attributes)
{
	TSharedRef<FJsonObject> NewMeta(new FJsonObject());
	AddAnalyticsEventAttributesToJsonObject(NewMeta, Attributes);
	FScopeLock WriteLock(&DataCriticalSection);
	MetaAttributes = NewMeta;
}

bool FsparklogsAnalyticsProvider::AutoStartSessionBeforeEvent()
{
	// Always start a session if we don't have one active, even on servers
	return StartSession(TEXT("auto started when first analytics event queued"), TArray<FAnalyticsEventAttribute>());
}

void FsparklogsAnalyticsProvider::AutoCleanupSession()
{
	if (IsRunningDedicatedServer())
	{
		return;
	}
	EndSession();
}

void FsparklogsAnalyticsProvider::CheckForStaleSessionAtStartup()
{
	FDateTime LastWrittenEvent = Settings->GetEffectiveLastWrittenAnalyticsEvent();
	FDateTime LastSessionStarted = ITLEmptyDateTime;
	FString LastSessionID;
	Settings->GetLastAnalyticsSessionStartInfo(LastSessionID, LastSessionStarted);
	if (!LastSessionID.IsEmpty() && LastSessionStarted != ITLEmptyDateTime)
	{
		// we shouldn't have an active session at this point, but in case we do, end it.
		EndSession();
		// simulate having the session that was last active to now be active
		int LastSessionNumber = Settings->GetSessionNumber(false);
		if (LastWrittenEvent == ITLEmptyDateTime)
		{
			LastWrittenEvent = LastSessionStarted;
		}
		FScopeLock WriteLock(&DataCriticalSection);
		CurrentSessionID = LastSessionID;
		SessionStarted = LastSessionStarted;
		SessionNumber = LastSessionNumber;
		WriteLock.Unlock();
		// end the session with the effective end time matching the time of last analytics event in that session
		DoEndSession(TEXT("stale session found at next game activation"), LastWrittenEvent);
		Settings->MarkEndOfAnalyticsSession();
	}
}

void FsparklogsAnalyticsProvider::GetAnalyticsEventData(FString& OutSessionID, FDateTime& OutSessionStarted, int& OutSessionNumber, TSharedPtr<FJsonObject>& OutMetaAttributes) const
{
	FScopeLock ReadLock(&DataCriticalSection);
	OutSessionID = CurrentSessionID;
	OutSessionStarted = SessionStarted;
	OutSessionNumber = SessionNumber;
	OutMetaAttributes = TSharedPtr<FJsonObject>(new FJsonObject(*MetaAttributes));
}

void FsparklogsAnalyticsProvider::SetMetaAttribute(const FString& Field, const TSharedPtr<FJsonValue> Value)
{
	FScopeLock WriteLock(&DataCriticalSection);
	MetaAttributes->SetField(Field, Value);
}

int FsparklogsAnalyticsProvider::GetSessionNumber()
{
	FScopeLock ReadLock(&DataCriticalSection);
	return SessionNumber;
}

void FsparklogsAnalyticsProvider::SetSessionNumber(int N)
{
	// This should really only be used on the server
	FScopeLock WriteLock(&DataCriticalSection);
	SessionNumber = N;
}

FDateTime FsparklogsAnalyticsProvider::GetSessionStarted()
{
	FScopeLock ReadLock(&DataCriticalSection);
	return SessionStarted;
}

void FsparklogsAnalyticsProvider::SetSessionStarted(FDateTime DT)
{
	// This should really only be used on the server
	FScopeLock WriteLock(&DataCriticalSection);
	SessionStarted = DT;
}

void FsparklogsAnalyticsProvider::FinalizeAnalyticsEvent(const TCHAR* EventType, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession, TSharedPtr<FJsonObject>& Object)
{
	FScopeLock ReadLock(&DataCriticalSection);
	InternalFinalizeAnalyticsEvent(EventType, OverrideSession, Object);
}

void FsparklogsAnalyticsProvider::InternalFinalizeAnalyticsEvent(const TCHAR* EventType, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession, TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
	{
		Object = TSharedPtr<FJsonObject>(new FJsonObject());
	}

	if (EventType != nullptr)
	{
		Object->SetStringField(StandardFieldEventType, EventType);
	}

	FString GameID = Settings->AnalyticsGameID;
	FString UserID = Settings->GetEffectiveAnalyticsUserID();
	FString PlayerID = Settings->GetEffectiveAnalyticsPlayerID();
	FString EffectiveSessionID = CurrentSessionID;
	FDateTime EffectiveSessionStarted = SessionStarted;
	int EffectiveSessionNumber = SessionNumber;
	TSharedPtr<FJsonObject> EffectiveMetaAttributes(new FJsonObject(*MetaAttributes));

	if (OverrideSession != nullptr)
	{
		EffectiveSessionID = OverrideSession->SessionID;
		EffectiveSessionNumber = OverrideSession->SessionNumber;
		EffectiveSessionStarted = OverrideSession->SessionStarted;
		UserID = OverrideSession->UserID;
		PlayerID = FsparklogsSettings::CalculatePlayerID(GameID, UserID);
	}
	if (EffectiveSessionID.IsEmpty() || GameID.IsEmpty() || UserID.IsEmpty() || PlayerID.IsEmpty())
	{
		// We cannot send an analytics event without a valid session ID, game ID, user ID, and player ID
		Object.Reset();
		return;
	}

	Object->SetStringField(StandardFieldSessionId, EffectiveSessionID);
	if (EffectiveSessionNumber > 0)
	{
		Object->SetNumberField(StandardFieldSessionNumber, (double)EffectiveSessionNumber);
	}
	if (EffectiveSessionStarted != ITLEmptyDateTime)
	{
		Object->SetStringField(StandardFieldSessionStarted, ITLGetUTCDateTimeAsRFC3339(EffectiveSessionStarted));
	}
	Object->SetStringField(StandardFieldSessionType, GetITLLaunchConfiguration(false));
	Object->SetStringField(StandardFieldGameId, GameID);
	Object->SetStringField(StandardFieldUserId, UserID);
	Object->SetStringField(StandardFieldPlayerId, PlayerID);

	FDateTime InstallTime = Settings->GetEffectiveAnalyticsInstallTime();
	Object->SetStringField(StandardFieldFirstInstalled, ITLGetUTCDateTimeAsRFC3339(InstallTime));

	if (EffectiveMetaAttributes.IsValid())
	{
		Object->SetObjectField(StandardFieldMeta, EffectiveMetaAttributes);
	}
}

FString FsparklogsAnalyticsProvider::CalculateFinalMessage(const FString& DefaultMessage, bool IncludeDefaultMessage, const TCHAR* ExtraMessage)
{
	FString FinalMessage;
	if (IncludeDefaultMessage)
	{
		FinalMessage = DefaultMessage;
	}
	if (ExtraMessage != nullptr && *ExtraMessage != 0)
	{
		if (!FinalMessage.IsEmpty())
		{
			FinalMessage += TEXT(" ");
		}
		FinalMessage += ExtraMessage;
	}
	return FinalMessage;
}

void FsparklogsAnalyticsProvider::AddAnalyticsEventAttributeToJsonObject(const TSharedPtr<FJsonObject> Object, const FAnalyticsEventAttribute& Attr, int AttrNumber)
{
	if (!Object.IsValid())
	{
		return;
	}
	FString AttrName = Attr.GetName().TrimStartAndEnd();
	if (AttrName.IsEmpty())
	{
		AttrName = ITLCalcUniqueFieldName(Object, TEXT("Custom"), AttrNumber);
	}
	if (Attr.IsJsonFragment())
	{
		TSharedPtr<FJsonValue> FragmentValue;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Attr.GetValue());
		if (FJsonSerializer::Deserialize(Reader, FragmentValue) && FragmentValue.IsValid())
		{
			Object->SetField(AttrName, FragmentValue);
			return;
		}
	}
	Object->SetStringField(AttrName, Attr.GetValue());
}

void FsparklogsAnalyticsProvider::AddAnalyticsEventAttributesToJsonObject(const TSharedPtr<FJsonObject> Object, const TArray<FAnalyticsEventAttribute>& EventAttrs)
{
	int AttrNumber = 1;
	for (const FAnalyticsEventAttribute& Attr : EventAttrs)
	{
		AddAnalyticsEventAttributeToJsonObject(Object, Attr, AttrNumber);
		AttrNumber++;
	}
}

void FsparklogsAnalyticsProvider::AddAnalyticsEventAttributeToJsonObject(const TSharedPtr<FJsonObject> Object, const FsparklogsAnalyticsAttribute& Attr, int AttrNumber)
{
	if (!Object.IsValid())
	{
		return;
	}
	FString AttrName = Attr.Key.TrimStartAndEnd();
	if (AttrName.IsEmpty())
	{
		AttrName = ITLCalcUniqueFieldName(Object, TEXT("Custom"), AttrNumber);
	}
	if (Attr.Value.Len() >= 2 && Attr.Value[0] == TEXT('{') && Attr.Value[Attr.Value.Len() - 1] == TEXT('}'))
	{
		TSharedPtr<FJsonValue> FragmentValue;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Attr.Value);
		if (FJsonSerializer::Deserialize(Reader, FragmentValue) && FragmentValue.IsValid())
		{
			Object->SetField(AttrName, FragmentValue);
			return;
		}
	}
	Object->SetStringField(AttrName, Attr.Value);
}

void FsparklogsAnalyticsProvider::AddAnalyticsEventAttributesToJsonObject(const TSharedPtr<FJsonObject> Object, const TArray<FsparklogsAnalyticsAttribute>& EventAttrs)
{
	int AttrNumber = 1;
	for (const FsparklogsAnalyticsAttribute& Attr : EventAttrs)
	{
		AddAnalyticsEventAttributeToJsonObject(Object, Attr, AttrNumber);
		AttrNumber++;
	}
}

void FsparklogsAnalyticsProvider::SetupDefaultMetaAttributes()
{
	FString OSPlatform, OSVersion;
	ITLGetOSPlatformVersion(OSPlatform, OSVersion);
	MetaAttributes->SetStringField(MetaFieldPlatform, OSPlatform);
	MetaAttributes->SetStringField(MetaFieldOSVersion, OSVersion);
		
	constexpr const TCHAR* EngineType = TEXT("unreal-");
	FString SDKVersion = TEXT("?");
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(ITL_PLUGIN_MODULE_NAME);
	if (Plugin.IsValid())
	{
		SDKVersion = FString(EngineType) + FString(TEXT("plugin-")) + Plugin->GetDescriptor().VersionName;
	}
	MetaAttributes->SetStringField(MetaFieldSDKVersion, SDKVersion);
	
	FEngineVersion EngineVersion = FEngineVersion::Current();
	MetaAttributes->SetStringField(MetaFieldEngineVersion, EngineType + EngineVersion.ToString(EVersionComponent::Patch));

	MetaAttributes->SetStringField(MetaFieldBuild, FApp::GetBuildVersion());

	TArray<FString> MMParts;
	FString MM = FPlatformMisc::GetDeviceMakeAndModel();
	MM.ParseIntoArray(MMParts, TEXT("|"), true);
	while (MMParts.Num() > 2)
	{
		MMParts[MMParts.Num() - 2] += TEXT(" ") + MMParts[MMParts.Num() - 1];
		MMParts.RemoveAt(MMParts.Num() - 1);
	}
	if (MMParts.Num() >= 1)
	{
		MetaAttributes->SetStringField(MetaFieldDeviceMake, MMParts[0]);
	}
	if (MMParts.Num() >= 2)
	{
		MetaAttributes->SetStringField(MetaFieldDeviceModel, MMParts[1]);
	}

	FString NetConnType = ITLGetNetworkConnectionType();
	if (!NetConnType.IsEmpty())
	{
		MetaAttributes->SetStringField(MetaFieldConnectionType, NetConnType);
	}
}

// =============== FsparklogsModule ===============================================================================

TSharedPtr<FsparklogsAnalyticsProvider> FsparklogsModule::AnalyticsProvider;

FsparklogsModule::FsparklogsModule()
	: EngineActive(false)
	, Settings(new FsparklogsSettings())
{
	GameInstanceID = ITLGenerateRandomAlphaNumID(24);
}

void FsparklogsModule::StartupModule()
{
	FCoreDelegates::OnPostEngineInit.AddRaw(this, &FsparklogsModule::OnPostEngineInit);
	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddRaw(this, &FsparklogsModule::OnAppEnterBackground);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddRaw(this, &FsparklogsModule::OnAppEnterForeground);
	// TODO: does it matter if we are loaded later and miss a bunch of log entries during engine initialization?
	// TODO: Should run plugin earlier and check command line to determine if this is running in an editor with
	//       similar logic to FEngineLoop::PreInitPreStartupScreen [LaunchEngineLoop.cpp] (GIsEditor not available earlier).
	//       If we change it here, also change GetITLLaunchConfiguration.
	IConsoleVariable* ICVar = IConsoleManager::Get().FindConsoleVariable(TEXT("log.Timestamp"), false);
	if (GIsEditor)
	{
		// We must force date/times to be logged in either UTC or Local so that each log message contains a timestamp.
		FString DefaultEngineIniPath = FPaths::ProjectConfigDir() + TEXT("DefaultEngine.ini");
		FString CurrentLogTimesValue = GConfig->GetStr(TEXT("LogFiles"), TEXT("LogTimes"), DefaultEngineIniPath).TrimStartAndEnd();
		if (CurrentLogTimesValue.Len() > 0 && CurrentLogTimesValue != TEXT("UTC") && CurrentLogTimesValue != TEXT("Local")) {
			UE_LOG(LogPluginSparkLogs, Warning, TEXT("Timestamps in log messages are required (LogTimes must be UTC or Local). Changing DefaultEngine.ini so [LogFiles]LogTimes=UTC"));
			GConfig->SetString(TEXT("LogFiles"), TEXT("LogTimes"), TEXT("UTC"), DefaultEngineIniPath);
			GPrintLogTimes = ELogTimes::UTC;
			if (ICVar)
			{
				ICVar->Set((int)ELogTimes::UTC, ECVF_SetByCode);
			}
		}
	}
	else
	{
		if (ICVar)
		{
			// Has to be either Local or UTC, force UTC if needed
			ELogTimes::Type CurrentValue = (ELogTimes::Type)ICVar->GetInt();
			if (CurrentValue != ELogTimes::UTC && CurrentValue != ELogTimes::Local)
			{
				UE_LOG(LogPluginSparkLogs, Warning, TEXT("SparkLogsPlugin: log.Timestamp not set to either Local or UTC; forcing to UTC"));
				ICVar->Set((int)ELogTimes::UTC, ECVF_SetByCode);
			}
		}
	}

	Settings->LoadSettings();
	if (Settings->AutoStart)
	{
		FSparkLogsEngineOptions DefaultOptions;
		StartShippingEngine(DefaultOptions);
	}
	else
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("AutoStart is disabled. Waiting for call to FsparklogsModule::GetModule().StartShippingEngine(...)"));
	}
}

void FsparklogsModule::ShutdownModule()
{
	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.RemoveAll(this);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.RemoveAll(this);
	FCoreDelegates::OnPostEngineInit.RemoveAll(this);
	FCoreDelegates::OnExit.RemoveAll(this);
	if (UObjectInitialized())
	{
		UnregisterSettings();
	}
	// Just in case it was not called earlier...
	StopShippingEngine();
}

TSharedPtr<IAnalyticsProvider> FsparklogsModule::CreateAnalyticsProvider(const FAnalyticsProviderConfigurationDelegate& GetConfigValue) const
{
	return GetAnalyticsProvider();
}

TSharedPtr<FsparklogsAnalyticsProvider> FsparklogsModule::GetAnalyticsProvider()
{
	if (!AnalyticsProvider.IsValid())
	{
		AnalyticsProvider = TSharedPtr<FsparklogsAnalyticsProvider>(new FsparklogsAnalyticsProvider(FsparklogsModule::GetModule().Settings));
	}
	return AnalyticsProvider;
}

bool FsparklogsModule::AddRawAnalyticsEvent(TSharedPtr<FJsonObject> RawAnalyticsData, const TCHAR* LogMessage, TSharedPtr<FJsonObject> CustomRootFields, bool ForceDisableAutoExtract, bool ForceDebugLogEvent)
{
	if (!EngineActive || !Settings->CollectAnalytics || !RawAnalyticsData.IsValid())
	{
		return false;
	}

	TSharedRef<FJsonObject> RootEvent(new FJsonObject());
	if (CustomRootFields.IsValid())
	{
		for (auto it = CustomRootFields->Values.CreateConstIterator(); it; ++it)
		{
			RootEvent->SetField(it->Key, it->Value);
		}
	}
	RootEvent->SetObjectField(FsparklogsAnalyticsProvider::RootAnalyticsFieldName, RawAnalyticsData);
	if (ForceDisableAutoExtract)
	{
		RootEvent->SetBoolField(OverrideAutoExtractDisabled, true);
	}
	FString OutputJson;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputJson);
	if (!FJsonSerializer::Serialize(RootEvent, Writer))
	{
		return false;
	}
	Writer->Close();
	if (Settings->DebugLogForAnalyticsEvents || ForceDebugLogEvent)
	{
		if (LogMessage != nullptr)
		{
			UE_LOG(LogPluginSparkLogs, Display, TEXT("%s: %s %s"), DebugForAnalyticsEventsPrefix, LogMessage, *OutputJson);
		}
		else
		{
			UE_LOG(LogPluginSparkLogs, Display, TEXT("%s: %s"), DebugForAnalyticsEventsPrefix, *OutputJson);
		}
	}
	Settings->MarkLastWrittenAnalyticsEvent();
	return GetITLInternalGameLog(nullptr).LogDevice->AddRawEventWithJSONObject(OutputJson, LogMessage, true);
}

FString FsparklogsModule::GetGameInstanceID()
{
	return GameInstanceID;
}

bool FsparklogsModule::StartShippingEngine(const FSparkLogsEngineOptions& options)
{
	if (EngineActive)
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Event shipping engine is already active. Ignoring call to StartShippingEngine."));
		return true;
	}

	// Lock in the application install date early even if analytics is not necessarily enabled yet
	Settings->GetEffectiveAnalyticsInstallTime();

	if (options.OverrideAnalyticsUserID.Len() > 0)
	{
		Settings->SetUserID(*options.OverrideAnalyticsUserID);
	}
	bool EffectiveCollectLogs = Settings->CollectLogs;
	if (options.OverrideCollectLogs != ESparkLogsOverrideBool::Default)
	{
		EffectiveCollectLogs = options.OverrideCollectLogs == ESparkLogsOverrideBool::True;
		// Temporarily override the setting in memory
		Settings->CollectLogs = EffectiveCollectLogs;
	}
	bool EffectiveCollectAnalytics = Settings->CollectAnalytics;
	if (options.OverrideCollectAnalytics != ESparkLogsOverrideBool::Default)
	{
		EffectiveCollectAnalytics = options.OverrideCollectAnalytics == ESparkLogsOverrideBool::True;
		// Temporarily override the setting in memory
		Settings->CollectAnalytics = EffectiveCollectAnalytics;
	}

	FString EffectiveAgentID = Settings->AgentID;
	FString EffectiveAgentAuthToken = Settings->AgentAuthToken;
	FString EffectiveHttpAuthorizationHeaderValue = Settings->HttpAuthorizationHeaderValue;
	if (options.OverrideAgentID.Len() > 0)
	{
		EffectiveAgentID = options.OverrideAgentID;
	}
	if (options.OverrideAgentAuthToken.Len() > 0)
	{
		EffectiveAgentAuthToken = options.OverrideAgentAuthToken;
	}
	if (options.OverrideHttpAuthorizationHeaderValue.Len() > 0)
	{
		EffectiveHttpAuthorizationHeaderValue = options.OverrideHttpAuthorizationHeaderValue;
	}

	bool UsingSparkLogsCloud = !Settings->CloudRegion.IsEmpty();
	FString EffectiveHttpEndpointURI = Settings->GetEffectiveHttpEndpointURI(options.OverrideHTTPEndpointURI);
	if (EffectiveHttpEndpointURI.IsEmpty())
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Not yet configured for this launch configuration. In plugin settings for %s launch configuration, configure CloudRegion to 'us' or 'eu' for your SparkLogs cloud region (or if you are sending data to your own HTTP service, configure HttpEndpointURI to the appropriate endpoint, such as http://localhost:9880/ or https://ingestlogs.myservice.com/ingest/v1)"), *GetITLINISettingPrefix());
		return false;
	}
	if (UsingSparkLogsCloud && (EffectiveAgentID.IsEmpty() || EffectiveAgentAuthToken.IsEmpty()))
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Not yet configured for this launch configuration. In plugin settings for %s launch configuration, configure authentication credentials to enable. Consider using credentials for Editor vs Client vs Server."), *GetITLINISettingPrefix());
		return false;
	}

	// If we're sending data to the SparkLogs cloud then use lz4 compression by default, otherwise use none as lz4 support is nonstandard.
	if (Settings->CompressionMode == ITLCompressionMode::Default)
	{
		if (UsingSparkLogsCloud || (!EffectiveAgentID.IsEmpty() && !EffectiveAgentAuthToken.IsEmpty()))
		{
			UE_LOG(LogPluginSparkLogs, Log, TEXT("Sending data to SparkLogs cloud, so using lz4 as default compression mode."));
			Settings->CompressionMode = ITLCompressionMode::LZ4;
		}
		else
		{
			UE_LOG(LogPluginSparkLogs, Log, TEXT("Sending data to custom HTTP destination, so using none as default compression mode."));
			Settings->CompressionMode = ITLCompressionMode::None;
		}
	}

	if (!FPlatformProcess::SupportsMultithreading())
	{
		UE_LOG(LogPluginSparkLogs, Warning, TEXT("This plugin cannot run on this platform. This platform does not multithreading."));
		return false;
	}

	if (!EffectiveCollectLogs && !EffectiveCollectAnalytics)
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Log collection and analytics collection are both disabled. No reason to start engine."));
		return false;
	}

	float DiceRoll = options.AlwaysStart ? 10000.0 : FMath::FRandRange(0.0, 100.0);
	EngineActive = DiceRoll < Settings->ActivationPercentage;
	if (EngineActive)
	{
		// Log all plugin messages to the ITL operations log
		GLog->AddOutputDevice(GetITLInternalOpsLog().LogDevice.Get());
		if (EffectiveCollectLogs)
		{
			// Log all engine messages to an internal log just for this plugin, which we will then read from the file as we push log data to the cloud
			GLog->AddOutputDevice(GetITLInternalGameLog(nullptr).LogDevice.Get());
		}
	}
	UE_LOG(LogPluginSparkLogs, Log, TEXT("Starting up: LaunchConfiguration=%s, HttpEndpointURI=%s, AgentID=%s, ActivationPercentage=%lf, DiceRoll=%f, Activated=%s, CollectLogs=%s, CollectAnalytics=%s"), GetITLLaunchConfiguration(true), *EffectiveHttpEndpointURI, *EffectiveAgentID, Settings->ActivationPercentage, DiceRoll, EngineActive ? TEXT("yes") : TEXT("no"), EffectiveCollectLogs ? TEXT("yes") : TEXT("no"), EffectiveCollectAnalytics ? TEXT("yes") : TEXT("no"));
	if (EngineActive)
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Ingestion parameters: RequestTimeoutSecs=%lf, BytesPerRequest=%d, ProcessingIntervalSecs=%lf, RetryIntervalSecs=%lf, UnflushedBytesToAutoFlush=%d, MinIntervalBetweenFlushes=%lf"), Settings->RequestTimeoutSecs, Settings->BytesPerRequest, Settings->ProcessingIntervalSecs, Settings->RetryIntervalSecs, (int)Settings->UnflushedBytesToAutoFlush, Settings->MinIntervalBetweenFlushes);
		if (EffectiveCollectAnalytics)
		{
			UE_LOG(LogPluginSparkLogs, Log, TEXT("Analytics collection is active. GameID='%s' UserID='%s' PlayerID='%s' DebugLogAllAnalyticsEvents=%s"), *Settings->AnalyticsGameID, *Settings->GetEffectiveAnalyticsUserID(), *Settings->GetEffectiveAnalyticsPlayerID(), Settings->DebugLogForAnalyticsEvents ? TEXT("true") : TEXT("false"));
			// Make sure analytics provider singleton is created and make sure any previously open session from a prior game instance is cleaned up...
			GetAnalyticsProvider()->CheckForStaleSessionAtStartup();
			if (ITLIsMobilePlatform() && Settings->AnalyticsMobileAutoSessionStart)
			{
				GetAnalyticsProvider()->StartSession(TEXT("automatically started at app start"), TArray<FAnalyticsEventAttribute>());
			}
		}
		
		FString SourceLogFile = GetITLInternalGameLog(nullptr).LogFilePath;
		FString AuthorizationHeader;
		if (EffectiveHttpAuthorizationHeaderValue.IsEmpty())
		{
			AuthorizationHeader = FString::Format(TEXT("Bearer {0}:{1}"), { *EffectiveAgentID, *EffectiveAgentAuthToken });
		}
		else
		{
			AuthorizationHeader = EffectiveHttpAuthorizationHeaderValue;
		}
		CloudPayloadProcessor = TSharedPtr<FsparklogsWriteHTTPPayloadProcessor, ESPMode::ThreadSafe>(new FsparklogsWriteHTTPPayloadProcessor(*EffectiveHttpEndpointURI, *AuthorizationHeader, Settings->RequestTimeoutSecs, Settings->DebugLogRequests));
		CloudStreamer = TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe>(new FsparklogsReadAndStreamToCloud(SourceLogFile, Settings, CloudPayloadProcessor.ToSharedRef(), GMaxLineLength, options.OverrideComputerName, GameInstanceID, &options.AdditionalAttributes));
		CloudStreamer->SetWeakThisPtr(CloudStreamer);
		FCoreDelegates::OnExit.AddRaw(this, &FsparklogsModule::OnEngineExit);
		GetITLInternalGameLog(CloudStreamer).LogDevice->SetCloudStreamer(CloudStreamer);

		if (Settings->StressTestGenerateIntervalSecs > 0)
		{
			StressGenerator = MakeUnique<FsparklogsStressGenerator>(Settings);
		}
	}
	return EngineActive;
}

void FsparklogsModule::StopShippingEngine()
{
	if (EngineActive || CloudStreamer.IsValid())
	{
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Shutting down and flushing logs to cloud..."));
		// If an analytics session is active then end it
		GetAnalyticsProvider()->EndSession(TEXT("automatically ended at app exit"));
		
		GLog->Flush();
		if (StressGenerator.IsValid())
		{
			StressGenerator->Stop();
		}
		if (CloudStreamer.IsValid())
		{
			if (CloudPayloadProcessor.IsValid())
			{
				// Set the retry interval to something short so we don't delay shutting down the game...
				Settings->RetryIntervalSecs = 0.2;
				// When the engine is shutting down, wait no more than 6 seconds to flush the final log request
				CloudPayloadProcessor->SetTimeoutSecs(FMath::Min(Settings->RequestTimeoutSecs, 6.0));
			}
			FOutputDevice* LogDevice = GetITLInternalGameLog(nullptr).LogDevice.Get();
			LogDevice->Flush();
			bool LastFlushProcessedEverything = false;
			if (CloudStreamer->FlushAndWait(2, true, true, true, FsparklogsSettings::WaitForFlushToCloudOnShutdown, LastFlushProcessedEverything))
			{
				FString LogFilePath = GetITLInternalGameLog(nullptr).LogFilePath;
				UE_LOG(LogPluginSparkLogs, Log, TEXT("Flushed logs successfully. LastFlushedEverything=%d"), (int)LastFlushProcessedEverything);
				// Purge this plugin's logfile and delete the progress marker (fully flushed shutdown should start with an empty log next game session).
				GLog->RemoveOutputDevice(LogDevice);
				LogDevice->TearDown();
				if (LastFlushProcessedEverything)
				{
					UE_LOG(LogPluginSparkLogs, Log, TEXT("All logs fully shipped. Removing progress marker and local logfile %s"), *LogFilePath);
					IFileManager::Get().Delete(*LogFilePath, false, false, false);
					CloudStreamer->DeleteProgressMarker();
				}
			}
			else
			{
				UE_LOG(LogPluginSparkLogs, Log, TEXT("Flush failed or timed out during shutdown."));
				GLog->RemoveOutputDevice(LogDevice);
				LogDevice->TearDown();
				// NOTE: the progress marker would not have been updated, so we'll keep trying the next time
				// the game engine starts right from where we left off, so we shouldn't lose anything.
			}
			CloudStreamer.Reset();
		}
		CloudPayloadProcessor.Reset();
		StressGenerator.Reset();
		UE_LOG(LogPluginSparkLogs, Log, TEXT("Shutdown."));
		EngineActive = false;
	}
}

void FsparklogsModule::OnAppEnterBackground()
{
	if (ITLIsMobilePlatform() && Settings->AnalyticsMobileAutoSessionEnd)
	{
		GetAnalyticsProvider()->EndSession(TEXT("automatically ended"));
	}
	Flush();
}

void FsparklogsModule::OnAppEnterForeground()
{
	if (ITLIsMobilePlatform() && Settings->AnalyticsMobileAutoSessionStart)
	{
		GetAnalyticsProvider()->StartSession(TEXT("automatically started at app activation"), TArray<FAnalyticsEventAttribute>());
	}
}

void FsparklogsModule::Flush()
{
	if (!EngineActive)
	{
		return;
	}
	GLog->Flush();
	if (CloudStreamer.IsValid() && CloudPayloadProcessor.IsValid())
	{
		FOutputDevice* LogDevice = GetITLInternalGameLog(nullptr).LogDevice.Get();
		if (LogDevice != nullptr)
		{
			LogDevice->Flush();
		}
		CloudStreamer->RequestFlush();
	}
}

void FsparklogsModule::OnPostEngineInit()
{
	if (UObjectInitialized())
	{
		// Allow the user to edit settings in the project settings editor
		RegisterSettings();
	}
}

void FsparklogsModule::OnEngineExit()
{
	UE_LOG(LogPluginSparkLogs, Log, TEXT("OnEngineExit. Will shutdown the log shipping engine..."));
	StopShippingEngine();
}

void FsparklogsModule::RegisterSettings()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->RegisterSettings("Project", "Plugins", "SparkLogs",
			LOCTEXT("RuntimeSettingsName", "SparkLogs"),
			LOCTEXT("RuntimeSettingsDescription", "Configure the SparkLogs plugin"),
			GetMutableDefault<USparkLogsRuntimeSettings>());
	}
}

void FsparklogsModule::UnregisterSettings()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->UnregisterSettings("Project", "Plugins", TEXT("SparkLogs"));
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FsparklogsModule, sparklogs)
