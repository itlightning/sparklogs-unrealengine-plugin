// Copyright (C) 2024-2025 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Object.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IAnalyticsProvider.h"
#include "Interfaces/IAnalyticsProviderModule.h"
#include "HAL/Runnable.h"
#include "Interfaces/IHttpResponse.h"
#include "HttpModule.h"
#include "Misc/OutputDeviceFile.h"
#include "sparklogs.generated.h"

#define ITL_CONFIG_SECTION_NAME TEXT("/Script/sparklogs.SparkLogsRuntimeSettings")

#define ITL_PLUGIN_MODULE_NAME "sparklogs"

DECLARE_LOG_CATEGORY_EXTERN(LogPluginSparkLogs, Log, All);

#define ITL_INTERNAL_DEBUG_LOG_DATA 0
#define ITL_INTERNAL_DEBUG_LOGGING 0
#if ITL_INTERNAL_DEBUG_LOGGING == 1
#define ITL_DBG_UE_LOG(LogCategory, Verbosity, Format, ...) \
	UE_LOG(LogCategory, Verbosity, TEXT("DEBUGDEBUGDEBUG(%.3lf): " Format), FPlatformTime::Seconds(), ##__VA_ARGS__)
#else
#define ITL_DBG_UE_LOG(LogCategory, Verbosity, Format, ...) \
	do { } while (0);
#endif

/** Convenience function to convert UTF8 data to an FString. Can incur allocations so use sparingly or only on debug paths. */
SPARKLOGS_API FString ITLConvertUTF8(const void* Data, int Len);

/** Trim all instances of given character from start and end of given string, inline. Returns true if anything was removed. */
SPARKLOGS_API bool ITLFStringTrimCharStartEndInline(FString & s, TCHAR c);

/** Returns a text encoding of the given severity that will be a severity level recognized by the SparkLogs cloud. */
SPARKLOGS_API const TCHAR* ITLSeverityToString(ELogVerbosity::Type Verbosity);

/** Calculates the OS platform name (windows/linux/...) and major OS version number (leaves out build info or other misc version info) */
SPARKLOGS_API void ITLGetOSPlatformVersion(FString & OutPlatform, FString & OutMajorVersion);

/** Calculates a string representation */
SPARKLOGS_API FString ITLGetNetworkConnectionType();

/** Formats an FDateTime that is already in UTC time zone in RFC3339 format (with milliseconds) */
SPARKLOGS_API FString ITLGetUTCDateTimeAsRFC3339(const FDateTime& DT);

/** The type of data compression to use. */
enum class SPARKLOGS_API ITLCompressionMode
{
	Default = 0,
	LZ4 = 0,
	None = 1
};

/** The type of user ID to use for analytics. */
enum class SPARKLOGS_API ITLAnalyticsUserIDType
{
	DeviceID = 0,
	Generated = 1
};

SPARKLOGS_API bool ITLCompressData(ITLCompressionMode Mode, const uint8* InData, int InDataLen, TArray<uint8>& OutData);
SPARKLOGS_API bool ITLDecompressData(ITLCompressionMode Mode, const uint8* InData, int InDataLen, int InOriginalDataLen, TArray<uint8>& OutData);
SPARKLOGS_API FString ITLGenerateNewRandomID();
SPARKLOGS_API FString ITLGenerateRandomAlphaNumID(int Length);

/**
 * Manages plugin settings.
 */
class SPARKLOGS_API FsparklogsSettings
{
public:
	static const TCHAR* PluginStateSection;

	static constexpr TCHAR* AnalyticsUserIDTypeDeviceID = TEXT("device_id");
	static constexpr TCHAR* AnalyticsUserIDTypeGenerated = TEXT("generated");
	static constexpr TCHAR* DefaultAnalyticsUserIDType = AnalyticsUserIDTypeDeviceID;

	static constexpr double DefaultRequestTimeoutSecs = 90;
	static constexpr double MinRequestTimeoutSecs = 30;
	static constexpr double MaxRequestTimeoutSecs = 4 * 60;
	static constexpr double DefaultActivationPercentage = 100.0;
	static constexpr int DefaultBytesPerRequest = 3 * 1024 * 1024;
	static constexpr int MinBytesPerRequest = 1024 * 128;
	static constexpr int MaxBytesPerRequest = 1024 * 1024 * 6;
	static constexpr int DefaultUnflushedBytesToAutoFlush = 1024 * 128;
	static constexpr int MinUnflushedBytesToAutoFlush = 1024 * 16;
	static constexpr double MinMinIntervalBetweenFlushes = 1.0;
	static constexpr double DefaultMinIntervalBetweenFlushes = 2.0;
	static constexpr double MinProcessingIntervalSecs = 0.5;
	static constexpr double DefaultRetryIntervalSecs = 30.0;
	static constexpr double MinRetryIntervalSecs = 15.0;
	// This should not be longer than 5 minutes, because the ingest dedup cache expires a few minutes later
	static constexpr double MaxRetryIntervalSecs = 5 * 60;
	static constexpr double WaitForFlushToCloudOnShutdown = 15.0;
	static constexpr bool DefaultIncludeCommonMetadata = true;
	static constexpr bool DefaultDebugLogRequests = false;
	static constexpr bool DefaultAutoStart = true;
	static constexpr bool DefaultAddRandomGameInstanceID = true;

	static constexpr double DefaultServerProcessingIntervalSecs = 2.0;
	static constexpr double DefaultEditorProcessingIntervalSecs = 2.0;
	// There could be millions of clients, so give more time for data to queue up before flushing...
	static constexpr double DefaultClientProcessingIntervalSecs = 60.0 * 10;

	static constexpr bool DefaultServerCollectAnalytics = true;
	static constexpr bool DefaultServerCollectLogs = true;
	static constexpr bool DefaultEditorCollectAnalytics = true;
	static constexpr bool DefaultEditorCollectLogs = true;
	static constexpr bool DefaultClientCollectAnalytics = true;
	static constexpr bool DefaultClientCollectLogs = false;

	static FDateTime EmptyDateTime;

	/** The game ID to use for analytics. If set, will also be added to log events. */
	FString AnalyticsGameID;
	/** The type of user ID to use for analytics. */
	ITLAnalyticsUserIDType AnalyticsUserIDType;

	/** Whether or not game analytic events are collected */
	bool CollectAnalytics;
	/** Whether or not logs are collected */
	bool CollectLogs;
	/** The cloud region we want to send logs to, such as 'us' or 'eu' */
	FString CloudRegion;
	/** Overrides the URI of the endpoint to push log payloads to */
	FString HttpEndpointURI;
	/** Request timeout in seconds */
	double RequestTimeoutSecs;
	/** ID of the agent when pushing logs to the cloud */
	FString AgentID;
	/** Auth token associated with the agent when pushing logs to the cloud */
	FString AgentAuthToken;
	/** Overrides the HTTP Authorization header value directly (if specified, the AgentID and AgentAuthToken values will be ignored). Useful if you specify your own HTTP endpoint. */
	FString HttpAuthorizationHeaderValue;
	/** What percent of the time to activate this plugin (whether started automatically or manually). 0.0 to 100.0. Defaults to 100%. Useful for incrementally rolling out the plugin. */
	double ActivationPercentage;
	/** Desired maximum bytes to read and process at one time (one "chunk"). */
	int32 BytesPerRequest;
	/** Desired seconds between attempts to read and process a chunk. */
	double ProcessingIntervalSecs;
	/** The amount of time to wait after a failed request before retrying. */
	double RetryIntervalSecs;
	/** If there are at least this many unflushed bytes and it's been at least MinIntervalBetweenFlushes time, it will automatically trigger a flush. */
	int32 UnflushedBytesToAutoFlush;
	/** The minimum amount of time between automatic size-triggered flushes */
	double MinIntervalBetweenFlushes;
	/** Whether or not to include common metadata in each log event. */
	bool IncludeCommonMetadata;
	/** Whether or not to log requests */
	bool DebugLogRequests;
	/** Whether or not to automatically start the log/event shipping engine. */
	bool AutoStart;
	/** The type of data compression to use on the log payload. */
	ITLCompressionMode CompressionMode;
	/** Whether or not to automatically add a game_instance_id field with a random ID (set once at engine startup) */
	bool AddRandomGameInstanceID;

	/** If non-zero, then will generate fake logs periodically */
	double StressTestGenerateIntervalSecs;
	/** The number of log entries to generate every generation interval. */
	int StressTestNumEntriesPerTick;

	FsparklogsSettings();

	/** Loads the settings from the game engine INI section appropriate for this launch configuration (editor, client, server, etc). */
	void LoadSettings();

	/** Gets the effective HTTP endpoint URI (either using the overridden HTTP endpoint URI if non-empty, or using the HttpEndpointURI if configured, or the CloudRegion). Returns empty if not configured. */
	FString GetEffectiveHttpEndpointURI(const TCHAR* OverrideHTTPEndpointURI);

	/** Thread-safe. Returns the User ID to use based on the analytics user ID type. */
	FString GetEffectiveAnalyticsUserID();

	/** Thread-safe. Returns the Player ID to use based on the analytics user ID and the analytics game ID. */
	FString GetEffectiveAnalyticsPlayerID();

	/** Thread-safe. Returns the PlayerID generated for a given game ID and user ID. */
	static FString CalculatePlayerID(const FString& GameID, const FString& UserID);

	/** Thread-safe. Returns the UTC timestamp when the game was first played */
	FDateTime GetEffectiveAnalyticsInstallTime();

	/** Thread-safe. Sets a custom User ID to use. */
	void SetUserID(const TCHAR* UserID);

	/** Thread-safe. Returns the session number since the app was first installed. Optionally increment the session number. */
	int GetSessionNumber(bool Increment);

	/** Thread-safe. Returns the transaction number since the app was first installed. Optionally increment the transaction number. */
	int GetTransactionNumber(bool Increment);

	/** Thread-safe. Returns the attempt number for the given event ID (case insensitive) since the app was first installed.
	  * Optionally increment the attempt number and/or optionally delete it (will still return last (possibly incremented) value).
	  */
	int GetAttemptNumber(const FString& EventID, bool Increment, bool DeleteAfter);

protected:
	FCriticalSection CachedCriticalSection;
	FString CachedAnalyticsUserID;
	FString CachedAnalyticsPlayerID;
	FDateTime CachedAnalyticsInstallTime;
	int CachedAnalyticsSessionNumber;
	int CachedAnalyticsTransactionNumber;
	TMap<FString, int> CachedAnalyticsAttemptNumber;

	/** Enforces constraints upon any loaded setting values. */
	void EnforceConstraints();

	/** Returns whether or not a given ID has valid information. */
	static bool IsValidDeviceID(const FString& deviceId);
};

/**
 * Settings for the plugin that will be managed and edited by Unreal Engine
 */
UCLASS(config=Engine, DefaultConfig)
class SPARKLOGS_API USparkLogsRuntimeSettings : public UObject
{
	GENERATED_BODY()

public:
	// ------------------------------------------ ANALYTICS
	
	// ID to identify this game when storing analytics (can be any arbitrary string, e.g., "my_platformer_game"). Required for analytics data to be sent.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics", DisplayName = "Game ID")
	FString AnalyticsGameID;

	// The type of user ID for analytics. "Device ID" uses FPlatformMisc::GetDeviceId() (identifier for vendor (idvf) on iOS, Android ID on Android, see docs for requirements on use of this ID type). "Generated" randomly generates an ID and stores it with app config data. All methods failback to Generated as needed. The user ID can also be manually overriden when starting the engine.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Analytics", DisplayName = "Type of User ID", meta = (GetOptions = "GetAnalyticsUserIDTypeOptions"))
	FString AnalyticsUserIDType = FsparklogsSettings::DefaultAnalyticsUserIDType;

	UFUNCTION()
	static TArray<FString> GetAnalyticsUserIDTypeOptions()
	{
		return {
			FsparklogsSettings::AnalyticsUserIDTypeDeviceID,
			FsparklogsSettings::AnalyticsUserIDTypeGenerated,
		};
	}

	// ------------------------------------------ SERVER LAUNCH CONFIGURATION SETTINGS

	// Whether or not to collect analytics on server launch configurations. Defaults to true; however, on server, session and client identity are not automatic -- you must manually specify the session ID and user ID with each event you ingest.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Enable Analytics")
	bool ServerCollectAnalytics = FsparklogsSettings::DefaultServerCollectAnalytics;

	// Whether or not to collect logs on server launch configurations. Defaults to true.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Auto Start")
	bool ServerCollectLogs = FsparklogsSettings::DefaultServerCollectLogs;

	// Set to 'us' or 'eu' based on what your SparkLogs workspace is provisioned to use.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "SparkLogs Cloud Region")
	FString ServerCloudRegion;

	// For authentication: ID of the cloud agent that will receive the ingested log data.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "SparkLogs Agent ID")
	FString ServerAgentID;

	// For authentication: Auth token associated with the cloud agent receiving the log data.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "SparkLogs Agent Auth Token")
	FString ServerAgentAuthToken;

	// Normally leave blank and set CloudRegion. Overrides the URI of the endpoint to push log payloads to, e.g., http://localhost:9880/ or https://ingestlogs.myservice.com/ingest/v1
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Custom HTTP Endpoint URI")
	FString ServerHTTPEndpointURI;

	// Normally leave blank and set AgentID and AgentAuthToken. Overrides the HTTP Authorization header value directly. Useful if you specify your own HTTP endpoint. For example: Bearer mybearertokenvalue */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Custom HTTP Authorization Header Value")
	FString ServerHTTPAuthorizationHeaderValue;

	// Whether or not to automatically start the log/event shipping engine. If disabled, you must manually start the engine by calling FsparklogsModule::GetModule().StartShippingEngine(...);
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Auto Start")
	bool ServerAutoStart = FsparklogsSettings::DefaultAutoStart;

	// What percent of the time to activate this plugin when the engine starts (whether automatically or manually with StartShippingEngine). 0.0 to 100.0. Defaults to 100%. Useful for incrementally rolling out the plugin.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Activation Percentage")
	float ServerActivationPercentage = FsparklogsSettings::DefaultActivationPercentage;

	// HTTP request timeout in seconds.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Request Timeout in Seconds")
	float ServerRequestTimeoutSecs = FsparklogsSettings::DefaultRequestTimeoutSecs;

	// Whether or not to automatically add a random game_instance_id field (ID randomly chosen at engine startup).
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Add Random Game Instance ID")
	bool ServerAddRandomGameInstanceID = FsparklogsSettings::DefaultAddRandomGameInstanceID;

	// ------------------------------------------ EDITOR LAUNCH CONFIGURATION SETTINGS

	// Whether or not to collect analytics on editor launch configurations. Defaults to true. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Enable Analytics")
	bool EditorCollectAnalytics = FsparklogsSettings::DefaultEditorCollectAnalytics;

	// Whether or not to collect logs on editor launch configurations. Defaults to true. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Auto Start")
	bool EditorCollectLogs = FsparklogsSettings::DefaultEditorCollectLogs;

	// Set to 'us' or 'eu' based on what your SparkLogs workspace is provisioned to use. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "SparkLogs Cloud Region")
	FString EditorCloudRegion;

	// For authentication: ID of the cloud agent that will receive the ingested log data. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName="SparkLogs Agent ID")
	FString EditorAgentID;

	// For authentication: Auth token associated with the cloud agent receiving the log data. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName="SparkLogs Agent Auth Token")
	FString EditorAgentAuthToken;

	// Normally leave blank and set CloudRegion. Overrides the URI of the endpoint to push log payloads to, e.g., http://localhost:9880/ or https://ingestlogs.myservice.com/ingest/v1 [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Custom HTTP Endpoint URI")
	FString EditorHTTPEndpointURI;

	// Normally leave blank and set AgentID and AgentAuthToken. Overrides the HTTP Authorization header value directly. Useful if you specify your own HTTP endpoint. For example: Bearer mybearertokenvalue [EDITOR RESTART REQUIRED] */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Custom HTTP Authorization Header Value")
	FString EditorHTTPAuthorizationHeaderValue;

	// Whether or not to automatically start the log/event shipping engine. If disabled, you must manually start the engine by calling FsparklogsModule::GetModule().StartShippingEngine(...); [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Auto Start")
	bool EditorAutoStart = FsparklogsSettings::DefaultAutoStart;

	// What percent of the time to activate this plugin when the engine starts (whether automatically or manually with StartShippingEngine). 0.0 to 100.0. Defaults to 100%. Useful for incrementally rolling out the plugin. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName="Activation Percentage")
	float EditorActivationPercentage = FsparklogsSettings::DefaultActivationPercentage;

	// HTTP request timeout in seconds. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Request Timeout in Seconds")
	float EditorRequestTimeoutSecs = FsparklogsSettings::DefaultRequestTimeoutSecs;

	// Whether or not to automatically add a random game_instance_id field (ID randomly chosen at engine startup). [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Add Random Game Instance ID")
	bool EditorAddRandomGameInstanceID = FsparklogsSettings::DefaultAddRandomGameInstanceID;

	// ------------------------------------------ CLIENT LAUNCH CONFIGURATION SETTINGS

	// Whether or not to collect analytics on client launch configurations. Defaults to true.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Enable Analytics")
	bool ClientCollectAnalytics = FsparklogsSettings::DefaultClientCollectAnalytics;

	// Whether or not to collect logs on client launch configurations. Defaults to false.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Auto Start")
	bool ClientCollectLogs = FsparklogsSettings::DefaultClientCollectLogs;

	// Set to 'us' or 'eu' based on what your SparkLogs workspace is provisioned to use.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "SparkLogs Cloud Region")
	FString ClientCloudRegion;

	// For authentication: ID of the cloud agent that will receive the ingested log data.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "SparkLogs Agent ID")
	FString ClientAgentID;

	// For authentication: Auth token associated with the cloud agent receiving the log data.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "SparkLogs Agent Auth Token")
	FString ClientAgentAuthToken;

	// Normally leave blank and set CloudRegion. Overrides the URI of the endpoint to push log payloads to, e.g., http://localhost:9880/ or https://ingestlogs.myservice.com/ingest/v1
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Custom HTTP Endpoint URI")
	FString ClientHTTPEndpointURI;

	// Normally leave blank and set AgentID and AgentAuthToken. Overrides the HTTP Authorization header value directly. Useful if you specify your own HTTP endpoint. For example: Bearer mybearertokenvalue */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Custom HTTP Authorization Header Value")
	FString ClientHTTPAuthorizationHeaderValue;

	// Whether or not to automatically start the log/event shipping engine. If disabled, you must manually start the engine by calling FsparklogsModule::GetModule().StartShippingEngine(...);
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Auto Start")
	bool ClientAutoStart = FsparklogsSettings::DefaultAutoStart;

	// What percent of the time to activate this plugin when the engine starts (whether automatically or manually with StartShippingEngine). 0.0 to 100.0. Defaults to 100%. Useful for incrementally rolling out the plugin.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Activation Percentage")
	float ClientActivationPercentage = FsparklogsSettings::DefaultActivationPercentage;

	// HTTP request timeout in seconds.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Request Timeout in Seconds")
	float ClientRequestTimeoutSecs = FsparklogsSettings::DefaultRequestTimeoutSecs;

	// Whether or not to automatically add a random game_instance_id field (ID randomly chosen at engine startup).
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Add Random Game Instance ID")
	bool ClientAddRandomGameInstanceID = FsparklogsSettings::DefaultAddRandomGameInstanceID;

	// ------------------------------------------ SERVER LAUNCH CONFIGURATION ADVANCED SETTINGS

	// Target bytes to read and process at one time (one "chunk").
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "Bytes Per Request")
	int32 ServerBytesPerRequest = FsparklogsSettings::DefaultBytesPerRequest;

	// Target seconds between attempts to read and process a chunk.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "Processing Interval in Seconds")
	float ServerProcessingIntervalSecs = FsparklogsSettings::DefaultServerProcessingIntervalSecs;

	// The amount of time to wait after a failed request before retrying.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "Retry Interval in Seconds")
	float ServerRetryIntervalSecs = FsparklogsSettings::DefaultRetryIntervalSecs;

	// If there are at least this many unflushed bytes and it's been at least MinIntervalBetweenFlushes time, it will automatically trigger a flush.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "Unflushed Bytes To Auto Flush")
	int32 ServerUnflushedBytesToAutoFlush = FsparklogsSettings::DefaultUnflushedBytesToAutoFlush;

	// The minimum number of seconds between automatic size-triggered flushes
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "Min Interval Between Flushes in Seconds")
	float ServerMinIntervalBetweenFlushes = FsparklogsSettings::DefaultMinIntervalBetweenFlushes;

	// Whether or not to include common metadata (hostname, game name) in each log event.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "Include Common Metadata")
	bool ServerIncludeCommonMetadata = FsparklogsSettings::DefaultIncludeCommonMetadata;

	// How to compress the payload. Use 'lz4' or 'none'. Defaults to lz4. 'lz4' is normally more CPU efficient as it reduces the size of the TLS payload.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "Compression Mode")
	FString ServerCompressionMode;

	// For Debugging: Whether or not to log requests.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "DEBUG: Log All HTTP Request")
	bool ServerDebugLogRequests = FsparklogsSettings::DefaultDebugLogRequests;

	// ------------------------------------------ EDITOR LAUNCH CONFIGURATION ADVANCED SETTINGS

	// Target bytes to read and process at one time (one "chunk"). [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Bytes Per Request")
	int32 EditorBytesPerRequest = FsparklogsSettings::DefaultBytesPerRequest;

	// Target seconds between attempts to read and process a chunk. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Processing Interval in Seconds")
	float EditorProcessingIntervalSecs = FsparklogsSettings::DefaultEditorProcessingIntervalSecs;

	// The amount of time to wait after a failed request before retrying. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Retry Interval in Seconds")
	float EditorRetryIntervalSecs = FsparklogsSettings::DefaultRetryIntervalSecs;

	// If there are at least this many unflushed bytes and it's been at least MinIntervalBetweenFlushes time, it will automatically trigger a flush. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", DisplayName = "Unflushed Bytes To Auto Flush")
	int32 EditorUnflushedBytesToAutoFlush = FsparklogsSettings::DefaultUnflushedBytesToAutoFlush;

	// The minimum number of seconds between automatic size-triggered flushes. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", DisplayName = "Min Interval Between Flushes in Seconds")
	float EditorMinIntervalBetweenFlushes = FsparklogsSettings::DefaultMinIntervalBetweenFlushes;

	// Whether or not to include common metadata (hostname, game name) in each log event. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Include Common Metadata")
	bool EditorIncludeCommonMetadata = FsparklogsSettings::DefaultIncludeCommonMetadata;

	// How to compress the payload. Use 'lz4' or 'none'. Defaults to lz4. 'lz4' is normally more CPU efficient as it reduces the size of the TLS payload. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Compression Mode")
	FString EditorCompressionMode;

	// For Debugging: Whether or not to log requests. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "DEBUG: Log All HTTP Request")
	bool EditorDebugLogRequests = FsparklogsSettings::DefaultDebugLogRequests;

	// ------------------------------------------ CLIENT LAUNCH CONFIGURATION ADVANCED SETTINGS

	// Target bytes to read and process at one time (one "chunk").
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "Bytes Per Request")
	int32 ClientBytesPerRequest = FsparklogsSettings::DefaultBytesPerRequest;

	// Target seconds between attempts to read and process a chunk.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "Processing Interval in Seconds")
	float ClientProcessingIntervalSecs = FsparklogsSettings::DefaultClientProcessingIntervalSecs;

	// The amount of time to wait after a failed request before retrying.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "Retry Interval in Seconds")
	float ClientRetryIntervalSecs = FsparklogsSettings::DefaultRetryIntervalSecs;

	// If there are at least this many unflushed bytes and it's been at least MinIntervalBetweenFlushes time, it will automatically trigger a flush.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "Unflushed Bytes To Auto Flush")
	int32 ClientUnflushedBytesToAutoFlush = FsparklogsSettings::DefaultUnflushedBytesToAutoFlush;

	// The minimum number of seconds between automatic size-triggered flushes.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "Min Interval Between Flushes in Seconds")
	float ClientMinIntervalBetweenFlushes = FsparklogsSettings::DefaultMinIntervalBetweenFlushes;

	// Whether or not to include common metadata (hostname, game name) in each log event.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "Include Common Metadata")
	bool ClientIncludeCommonMetadata = FsparklogsSettings::DefaultIncludeCommonMetadata;

	// How to compress the payload. Use 'lz4' or 'none'. Defaults to lz4. 'lz4' is normally more CPU efficient as it reduces the size of the TLS payload.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "Compression Mode")
	FString ClientCompressionMode;

	// For Debugging: Whether or not to log requests.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "DEBUG: Log All HTTP Request")
	bool ClientDebugLogRequests = FsparklogsSettings::DefaultDebugLogRequests;
};

class SPARKLOGS_API FsparklogsReadAndStreamToCloud;

/**
 * An interface that takes a (potentially compressed) JSON log payload from the WORKER thread of the streamer, and processes it.
 */
class SPARKLOGS_API IsparklogsPayloadProcessor
{
public:
	virtual ~IsparklogsPayloadProcessor() = default;
	/** Processes the JSON payload, and returns true on success or false on failure. */
	virtual bool ProcessPayload(TArray<uint8>& JSONPayloadInUTF8, int PayloadLen, int OriginalPayloadLen, ITLCompressionMode CompressionMode, TWeakPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> StreamerWeakPtr) = 0;
};

/** A payload processor that writes the data to a local file (for DEBUG purposes only). */
class SPARKLOGS_API FsparklogsWriteNDJSONPayloadProcessor : public IsparklogsPayloadProcessor
{
protected:
	FString OutputFilePath;
public:
	FsparklogsWriteNDJSONPayloadProcessor(FString InOutputFilePath);
	virtual bool ProcessPayload(TArray<uint8>& JSONPayloadInUTF8, int PayloadLen, int OriginalPayloadLen, ITLCompressionMode CompressionMode, TWeakPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> StreamerWeakPtr) override;
};

/** A payload processor that synchronously POSTs the data to an HTTP(S) endpoint. */
class SPARKLOGS_API FsparklogsWriteHTTPPayloadProcessor : public IsparklogsPayloadProcessor
{
protected:
	FString EndpointURI;
	FString AuthorizationHeader;
	FThreadSafeCounter TimeoutMillisec;
	bool LogRequests;
public:
	FsparklogsWriteHTTPPayloadProcessor(const TCHAR* InEndpointURI, const TCHAR* InAuthorizationHeader, double InTimeoutSecs, bool InLogRequests);
	virtual bool ProcessPayload(TArray<uint8>& JSONPayloadInUTF8, int PayloadLen, int OriginalPayloadLen, ITLCompressionMode CompressionMode, TWeakPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> StreamerWeakPtr) override;
	void SetTimeoutSecs(double InTimeoutSecs);

protected:
	/** Sets an HTTP header to communicate proper timezone information */
	void SetHTTPTimezoneHeader(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest);
	/** Wait for the HTTP request to complete. Returns false on timeout or true if the request completed. */
	bool SleepWaitingForHTTPRequest(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest, FThreadSafeBool& RequestEnded, FThreadSafeBool& RequestSucceeded, FThreadSafeBool& RetryableFailure, double StartTime);
};

using TITLJSONStringBuilder = TAnsiStringBuilder<4 * 1024>;

/**
 * Background thread that generates fake log entries to stress the logging system.
 */
class SPARKLOGS_API FsparklogsStressGenerator : public FRunnable
{
protected:
	TSharedRef<FsparklogsSettings> Settings;
	volatile FRunnableThread* Thread;
	/** Non-zero stops this thread */
	FThreadSafeCounter StopRequestCounter;

public:
	FsparklogsStressGenerator(TSharedRef<FsparklogsSettings> InSettings);
	~FsparklogsStressGenerator();

	//~ Begin FRunnable Interface
	virtual bool Init();
	virtual uint32 Run();
	virtual void Stop();
	//~ End FRunnable Interface
};

/**
* On a background thread, reads data from a logfile on disk and streams to the cloud.
*/
class SPARKLOGS_API FsparklogsReadAndStreamToCloud : public FRunnable
{
protected:
	static const TCHAR* ProgressMarkerValue;

	TWeakPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> WeakThisPtr;
	TSharedRef<FsparklogsSettings> Settings;
	TSharedRef<IsparklogsPayloadProcessor, ESPMode::ThreadSafe> PayloadProcessor;
	FString ProgressMarkerPath;
	FString SourceLogFile;
	int MaxLineLength;

	/** If non-empty, will override the computer name */
	FString OverrideComputerName;
	/** JSON object fragment that is common to all log events (hostname, project name, etc.) */
	TArray<uint8> CommonEventJSONData;
	/** WORKER thread */
	volatile FRunnableThread* Thread;
	/** Non-zero stops this thread */
	FThreadSafeCounter StopRequestCounter;
	/** Non-zero indicates a request to flush to cloud */
	FThreadSafeCounter FlushRequestCounter;
	/** The number of times we've finished a flush to cloud (success or fail) */
	FThreadSafeCounter FlushOpCounter;
	/** The number of times we've successfully finished a flush to cloud */
	FThreadSafeCounter FlushSuccessOpCounter;
	/** Set to true if in the last flush everything in the log file was processed */
	FThreadSafeBool LastFlushProcessedEverything;
	/** Whether or not the worker fully cleaned up */
	FThreadSafeBool WorkerFullyCleanedUp;
	/** [WORKER] buffer to hold data for current chunk being processed. Will be BytesPerRequest in size. */
	TArray<uint8> WorkerBuffer;
	/** [WORKER] string buffer that holds JSON data for next payload to deliver to the cloud. Will be BytesPerRequest in size. */
	TITLJSONStringBuilder WorkerNextPayload;
	/** [WORKER] byte buffer that holds the encoded data for the next payload. Can vary in size based on compression mode. */
	TArray<uint8> WorkerNextEncodedPayload;
	/** [WORKER] The offset where we next need to start processing data in the logfile. */
	int64 WorkerShippedLogOffset;
	/** [WORKER] If non-zero, the minimum time when we can attempt to flush to cloud again automatically. Useful to wait longer to retry after a failure. */
	double WorkerMinNextFlushPlatformTime;
	/** [WORKER] The number of consecutive flush failures we've had in a row. */
	int WorkerNumConsecutiveFlushFailures;
	/** [WORKER] The payload size of the request the last time we failed to flush. */
	int WorkerLastFailedFlushPayloadSize;
	/** Whether or not the next flush platform time is because of a failure. */
	FThreadSafeBool WorkerLastFlushFailed;

	/** The time of the initiation of the last flush (or 0 if we have never flushed). */
	std::atomic<double> LastFlushPlatformTime;
	/** The amount of bytes queued up since last flush. */
	std::atomic<int64> BytesQueuedSinceLastFlush;

	virtual void ComputeCommonEventJSON(bool IncludeCommonMetadata, const TCHAR* GameInstanceID, TMap<FString, FString>* AdditionalAttributes);

public:

	FsparklogsReadAndStreamToCloud(const TCHAR* SourceLogFile, TSharedRef<FsparklogsSettings> InSettings, TSharedRef<IsparklogsPayloadProcessor, ESPMode::ThreadSafe> InPayloadProcessor, int InMaxLineLength, const TCHAR* InOverrideComputerName, const TCHAR* GameInstanceID, TMap<FString, FString>* AdditionalAttributes);
	~FsparklogsReadAndStreamToCloud();

	// After constructing this object you must give it a weak pointer to itself before running it. Needed to pass to payload processor.
	void SetWeakThisPtr(TWeakPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> P) {
		WeakThisPtr = P;
	}

	//~ Begin FRunnable Interface
	virtual bool Init();
	virtual uint32 Run();
	virtual void Stop();
	//~ End FRunnable Interface

	/** Thread-safe. Recognize that N bytes was just queued up (written to the backing file), and trigger a flush if appropriate
	  * (it's been at least X bytes since the last flush and it's been at least Y seconds). Returns true if flush happened. */
	virtual bool AccrueWrittenBytes(int N);

	/** Request that a Flush is initiated if possible. Does not wait for the flush to finish. Returns true if flush was initiated. */
	virtual bool RequestFlush();

	/** Initiate Flush up to N times, optionally clear retry timer to try again immediately, optionally initiate Stop, and wait up through a timeout for each flush to complete. Returns false on timeout or if the flush failed. */
	virtual bool FlushAndWait(int N, bool ClearRetryTimer, bool InitiateStop, bool OnMainGameThread, double TimeoutSec, bool& OutLastFlushProcessedEverything);

	/** Read the progress marker. Returns false on failure. */
	virtual bool ReadProgressMarker(int64& OutMarker);
	/** Writes the progress marker. Returns false on failure. */
	virtual bool WriteProgressMarker(int64 InMarker);
	/** Delete the progress marker */
	virtual void DeleteProgressMarker();

	/** [WORKER] Returns the number of seconds to wait during a flush retry based on the number of consecutive failures. */
	virtual double WorkerGetRetrySecs();

protected:
	/** [WORKER] Re-opens the logfile and reads more data into the work buffer. */
	virtual bool WorkerReadNextPayload(int& OutNumToRead, int64& OutEffectiveShippedLogOffset, int64& OutRemainingBytes);
	/** [WORKER] Build the JSON payload from as much of the data in WorkerBuffer as possible, up to NumToRead bytes. Sets OutCapturedOffset to the number of bytes captured into the payload. Returns false on failure. Do not call directly. */
	virtual bool WorkerBuildNextPayload(int NumToRead, int& OutCapturedOffset, int& OutNumCapturedLines);
	/** [WORKER] Compress the current payload in WorkerNextPayload and store in WorkerNextEncodedPayload. */
	virtual bool WorkerCompressPayload();
	/** [WORKER] Does the actual work for the flush operation, returns true on success. Does not update progress marker or thread state. Do not call directly. */
	virtual bool WorkerInternalDoFlush(int64& OutNewShippedLogOffset, bool& OutFlushProcessedEverything);
	/** [WORKER] Attempts to flush any newly available logs to the cloud. Response for updating flush op counters, LastFlushProcessedEverything, and MinNextFlushPlatformTime state. Returns false on failure. Only call from worker thread. */
	virtual bool WorkerDoFlush();
};

/**
* Custom log file output device for SparkLogs plugin. Special behaviors vs regular file output:
*  - Certain categories can be forced to log to the file even if (!ALLOW_LOG_FILE || NO_LOGGING). Used for analytics event encoding.
*  - Will encode the default log severity explicitly to make sure messages are not auto-detected as something else.
*  - Always UTF-8 and always append-only. Never backs up old logfiles.
*  - Will accrue bytes with the FsparklogsReadAndStreamToCloud to automatically trigger a flush if appropriate.
*/
class SPARKLOGS_API FsparklogsOutputDeviceFile : public FOutputDevice
{
public:
	/** Constructor. InFilename is required. If the file already exists, data will be appended to it. */
	FsparklogsOutputDeviceFile(const TCHAR* InFilename, TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> CloudStreamer);

	/** Deconstructor that performs cleanup. */
	~FsparklogsOutputDeviceFile();

	void SetCloudStreamer(TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> CloudStreamer);

	//~ Begin FOutputDevice Interface.
	/**
	* Closes output device and cleans up. This can't happen in the destructor
	* as we have to call "delete" which cannot be done for static/ global
	* objects.
	*/
	void TearDown() override;

	/**
	* Flush the write cache so the file isn't truncated in case we crash right
	* after calling this function.
	*/
	void Flush() override;

	virtual void Serialize(const TCHAR* Data, ELogVerbosity::Type Verbosity, const class FName& Category, const double Time) override;
	virtual void Serialize(const TCHAR* Data, ELogVerbosity::Type Verbosity, const class FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override
	{
		return true;
	}
	//~ End FOutputDevice Interface.

	/** Returns the filename that output data will be appended to. */
	const FString GetFilename() const { return Filename; }

	/** Returns True if the output file is open and ready to receive appended data. */
	bool IsOpened() const { return AsyncWriter != nullptr; }

	/** Add a category name to the "always logged" filter. */
	void AddAlwaysLoggedCategory(const class FName& InCategoryName) { AlwaysLoggedCategories.Add(InCategoryName); }

	/** Thread safe method to add one new event to the queue. Raw JSON can be specified (should be the contents of an
	  * encoded JSON object, but *without* the surrounding {}) as well as optional raw message text. You should include
	  * in the RawJSON a timestamp field with the time of the message, or a timestamp at the start of the message field.
	  * Returns true on success. */
	bool AddRawEvent(const TCHAR* RawJSON, const TCHAR* Message);

	/** Similar to AddRawEvent but the format of RawJSON expects that the start/end *do* including the curly braces {}.
	  * Can also optionally add the current UTC time to the JSON data.
	  */
	bool AddRawEventWithJSONObject(const FString& RawJSONWithBraces, const TCHAR* Message, bool AddUTCNow);

protected:
	/** Whether or not we've hit a failure that causes future writes to fail. */
	bool Failed;
	/** Whether or not to force a log flush after every log message. */
	bool ForceLogFlush;
	/** The path to the file we are writing to. */
	FString Filename;
	/** The async writer we use to actually write the file data. */
	FAsyncWriter* AsyncWriter;
	/** The output archive used by the async writer. */
	FArchive* WriterArchive;
	/** The categories that are always logged to the file, even if (!ALLOW_LOG_FILE || NO_LOGGING). */
	TSet<FName> AlwaysLoggedCategories;
	/** The weak reference to the streamer that will accrue bytes for auto-flushing. */
	TWeakPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> CloudStreamerWeakPtr;

	/** Writes out the given message event, potentially with a common message tag (e.g., date/time and verbosity/category), potentially with the ExtraJSON at the front. */
	static void InternalAddMessageEvent(FArchive& Output, FString& RawJSON, const TCHAR* Message, ELogVerbosity::Type Verbosity, const class FName& Category, const double Time, bool bSuppressEventTag);

	/** If the writer is not yet created, attempts to create it. On failure, sets Failed to True and returns false. */
	bool CreateAsyncWriter();
	
	/** Returns whether or not we are allowed to log a message with the given category.
	  * if (ALLOW_LOG_FILE && !NO_LOGGING) then this will always return true. Otherwise,
	  * it will return true only if the category name is in the always logged category set.
	  */
	bool ShouldLogCategory(const class FName& InCategoryName);

	/** Accrue N bytes to the cloud streamer if possible. */
	bool AccrueWrittenBytes(int N);
};

/** Uniquely identifies an analytics session. Pass this information from a client to a server
  * to allow the server to explicitly create analytics events on behalf of the client *without*
  * starting an analytics session on the server.
  * 
  * The bare minimum to have a valid session descriptor is the session ID and user ID.
  * If available, the session number and session start time will complete the optional info.
  */
class FSparkLogsAnalyticsSessionDescriptor {
public:
	FSparkLogsAnalyticsSessionDescriptor();
	FSparkLogsAnalyticsSessionDescriptor(const TCHAR* InSessionID, const TCHAR* InUserID);
	FSparkLogsAnalyticsSessionDescriptor(const TCHAR* InSessionID, int InSessionNumber, const TCHAR* InUserID);
	FSparkLogsAnalyticsSessionDescriptor(const TCHAR* InSessionID, int InSessionNumber, FDateTime InSessionStarted, const TCHAR* InUserID);
	virtual ~FSparkLogsAnalyticsSessionDescriptor();

	FString SessionID;
	int SessionNumber;
	FDateTime SessionStarted;
	FString UserID;
};

UENUM(BlueprintType)
enum class EsparklogsSeverity : uint8 {
	Trace UMETA(DisplayName = "Trace"),
	Debug UMETA(DisplayName = "Debug"),
	Info UMETA(DisplayName = "Info"),
	Notice UMETA(DisplayName = "Notice"),
	Warn UMETA(DisplayName = "Warn"),
	Error UMETA(DisplayName = "Error"),
	Critical UMETA(DisplayName = "Critical"),
	Fatal UMETA(DisplayName = "Fatal"),
	Alert UMETA(DisplayName = "Alert"),
	Panic UMETA(DisplayName = "Panic"),
	Emergency UMETA(DisplayName = "Emergency")
};

UENUM(BlueprintType)
enum class EsparklogsAnalyticsProgressionStatus : uint8 {
	Started UMETA(DisplayName = "Started"),
	Failed UMETA(DisplayName = "Failed"),
	Completed UMETA(DisplayName = "Completed")
};

UENUM(BlueprintType)
enum class EsparklogsAnalyticsFlowType : uint8 {
	Source UMETA(DisplayName = "Source"),
	Sink UMETA(DisplayName = "Sink")
};

USTRUCT(BlueprintType)
struct FsparklogsAnalyticsAttribute
{
	GENERATED_USTRUCT_BODY();

	FsparklogsAnalyticsAttribute() {}
	FsparklogsAnalyticsAttribute(const FString& K, const FString& V) { Key = K; Value = V; }
	~FsparklogsAnalyticsAttribute() {}

	UPROPERTY(Category = "SparkLogs", EditAnywhere, BlueprintReadWrite);
	FString Key;

	UPROPERTY(Category = "SparkLogs", EditAnywhere, BlueprintReadWrite);
	FString Value;
};

/**
 * Analytics class to generate various types of analytics events (purchase,
 * resource, progression, design, log). Static methods are thread-safe.
 * 
 * It's recommended to use this class to generate analytics events with more
 * detail and customization. You should also configure sparklogs as an analytics
 * provider to capture analytics events generated by Unreal Engine itself (see
 * detailed instructions below).
 *
 * Analytics must be enabled in the sparklogs module settings for this launch
 * configuration in order for the data to go anywhere.
 *
 * On the client, you must have an active analytics session to generate events.
 * Either rely on the plugin's session auto-start and auto-end features (when game
 * launches and when game finally exits and for mobile when app activates/deactivates),
 * or use StartSession and EndSession to start/end analytics sessions.
 *
 * If you're on a server and you want to record analytics events for a given client,
 * you should explicitly pass overridden session information when creating each
 * analytics event by passing a SparkLogsAnalyticsSessionDescriptor (recommended).
 * It is NOT recommended to use the Unreal Engine analytics plugin method SetSession
 * and SetUserID as these set server session information globally (NOT recommended
 * unless your server is single threaded! (highly unlikely)).
 * 
 * For item category, item ID, currencies, virtual currencies, progression tiers,
 * and design event IDs, there are no limits on cardinality and no configuration
 * with a list of valid values is required. However, in general you should avoid
 * arbitrary dynamic data in these values to make the resulting segmentation and
 * analysis more useful.
 *
 * To use sparklogs as an analytics provider, in DefaultEngine.ini do:
 * [Analytics]
 * ProviderModuleName=sparklogs
 *
 * Or to use it with other analytics providers, in DefaultEngine.ini do:
 * [Analytics]
 * ProviderModuleName=AnalyticsMulticast
 * ProviderModuleNames=sparklogs,...
 */
UCLASS()
class SPARKLOGS_API UsparklogsAnalytics : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	/** Starts a session (if one has already started, does nothing and treats as success). */
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static bool StartSession();

	/** Ends a session if any is active (if one is not active, does nothing) */
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void EndSession();

	/** Gets the ID of the current session, or returns an empty string if none is active. */
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static FString GetSessionID();

	/** Sets the build information about this version of the game to include in all analytics events. */
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void SetBuildInfo(const FString& InBuildInfo);

	/** Thread-safe. Sets the given field in the common attributes (added to all analytics events) to the given value.
	  * If the value is an object you should NEVER mutate it after storing it into the meta attributes. */
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void SetCommonAttribute(const FString& Field, const FString& Value);
	static void SetCommonAttributeJSON(const FString& Field, const TSharedPtr<FJsonValue> Value);

	/** Records real-money purchase events. ItemCategory and ItemId form a hierarchy.
	  *
	  * For example, the user might purchase additional "gems" (a virtual currency) through
	  * a 1000 gem pack, which could be identified with an ItemCategory of "in_app_purchase"
	  * and an Itemid of "1000_gem_pack". When a purchase occurs that generates in-game virtual
	  * currency then you should also record a Resource source event with the appropriate
	  * virtual currency for the same ItemCategory and ItemId.
	  *
	  * There are no cardinality limits on the combination of values for ItemCategory & ItemId,
	  * but we recommend you limit the category and item IDs to make your analysis more meaningful.
	  *
	  * RealCurrencyCode should be an alphabetic ISO 4217 code (e.g., USD, EUR, etc.) (if nullptr will assume USD).
	  * Amount should be the actual amount of that local currency. e.g., one dollar and 99 cents would be "1.99".
	  *
	  * You can optionally specify a reason that triggered the purchase.
	  *
	  * This will automatically add a transaction number field indicating the sequential order of the transaction
	  * since the game was installed, so you can analyze what players are purchasing in what order.
	  *
	  * You can optionally specify additional custom attributes by passing a non-null JSON object (or
	  * array of FsparklogsAnalyticsAttribute).
	  *
	  * You can optionally specify additional message text to be associated with the analytics event (instead of or
	  * in addition to a default message text that will describe this purchase event in natural language).
	  *
	  * If you're in a server process, then you may not have an active session, and instead you want to create an
	  * event that is associated with a session for a given client process. In that case, create a
	  * FSparkLogsAnalyticsSessionDescriptor specifying at least the session ID and user ID and pass to OverrideSessionInfo.
	  *
	  * Returns whether or not the event was created/queued.
	  */
	static bool AddPurchase(const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* RealCurrencyCode, double Amount, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddPurchase(const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* RealCurrencyCode, double Amount, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddPurchase(const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* RealCurrencyCode, double Amount, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddPurchase(const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* RealCurrencyCode, double Amount, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);

	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordPurchase(const FString& ItemCategory, const FString& ItemId, const FString& RealCurrencyCode, float Amount);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordPurchaseWithAttrs(const FString& ItemCategory, const FString& ItemId, const FString& RealCurrencyCode, float Amount, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordPurchaseWithReason(const FString& ItemCategory, const FString& ItemId, const FString& RealCurrencyCode, float Amount, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordPurchaseWithReasonWithAttrs(const FString& ItemCategory, const FString& ItemId, const FString& RealCurrencyCode, float Amount, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);

	/** Records the granting (resource source) or using up (resource sink) of a virtual currency (gems, lives, etc.)
	 * for a particular ItemCategory and ItemId, which form a hierarchy.
	 *
	 * The amount should be positive for sources and negatives for sinks. It will automatically enforce
	 * this using the absolute value of the amount as needed.
	 *
	 * There are no limits on the possible values for ItemCategory and ItemId, but in general be thoughtful
	 * about what values you use and how you will design your analysis.
	 *
	 * Also, be thoughtful about the balance between recording every resource source/sink event as it happens
	 * (which could be very often), vs aggregating the information and recording it only at key points (e.g.
	 * when losing a life or ending a level). The system can handle sending frequent events, but this could
	 * generate exponentially more data and could thus cost more at large scale. Be especially thoughtful
	 * when you have 100s of thousands of users or more.
	 *
	 * If a resource was granted as the result of a purchase using real money, then make sure to also record a
	 * corresponding purchase event as well with the same ItemCategory and ItemId.
	 *
	 * You can optionally specify an additional textual reason describing more information why the event occurred.
	 *
	 * If you're in a server process, then you may not have an active session, and instead you want to create an
	 * event that is associated with a session for a given client process. In that case, create a
	 * FSparkLogsAnalyticsSessionDescriptor specifying at least the session ID and user ID and pass to OverrideSessionInfo.
	 *
	 * Returns whether or not the event was created/queued.
	 */
	static bool AddResource(EsparklogsAnalyticsFlowType FlowType, double Amount, const TCHAR* VirtualCurrency, const TCHAR* ItemCategory, const TCHAR* ItemId, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddResource(EsparklogsAnalyticsFlowType FlowType, double Amount, const TCHAR* VirtualCurrency, const TCHAR* ItemCategory, const TCHAR* ItemId, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddResource(EsparklogsAnalyticsFlowType FlowType, double Amount, const TCHAR* VirtualCurrency, const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddResource(EsparklogsAnalyticsFlowType FlowType, double Amount, const TCHAR* VirtualCurrency, const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordResource(EsparklogsAnalyticsFlowType FlowType, float Amount, const FString& VirtualCurrency, const FString& ItemCategory, const FString& ItemId);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordResourceWithAttrs(EsparklogsAnalyticsFlowType FlowType, float Amount, const FString& VirtualCurrency, const FString& ItemCategory, const FString& ItemId, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordResourceWithReason(EsparklogsAnalyticsFlowType FlowType, float Amount, const FString& VirtualCurrency, const FString& ItemCategory, const FString& ItemId, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordResourceWithReasonWithAttrs(EsparklogsAnalyticsFlowType FlowType, float Amount, const FString& VirtualCurrency, const FString& ItemCategory, const FString& ItemId, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);

	/** Records a progression event, which records starting, failing, or completing a certain part of
	  * a game. Progression events form a hierarchy and can have up to N arbitrary tiers (world,
	  * region, level, segment, etc.).
	  *
	  * Later tiers are optional and can be an empty string or nullptr, but you cannot specify a later
	  * tier value if an earlier tier is not set.
	  *
	  * While there is no limit on depth or cardinality of the combination of tier values, be smart about
	  * how many total possible values of unique progression event IDs you create based on your planned analysis.
	  *
	  * Design events may optionally be associated with a numeric value as well.
	  *
	  * If you're in a server process, then you may not have an active session, and instead you want to create an
	  * event that is associated with a session for a given client process. In that case, create a
	  * FSparkLogsAnalyticsSessionDescriptor specifying at least the session ID and user ID and pass to OverrideSessionInfo.
	  *
	  * Returns whether or not the event was created/queued.
	  */
	static bool AddProgression1(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression2(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression3(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression4(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression5(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression1(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression2(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression3(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression4(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression5(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression1(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression2(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression3(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression4(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression5(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression1(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression2(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression3(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression4(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression5(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression1(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression2(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression3(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression4(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression5(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression1(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression2(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression3(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression4(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression5(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression1(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression2(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression3(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression4(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression5(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression1(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression2(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression3(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression4(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgression5(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, double Value, const TArray<FString>& PArray, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, double Value, const TArray<FString>& PArray, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, double Value, const TArray<FString>& PArray, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddProgressionArray(EsparklogsAnalyticsProgressionStatus Status, double Value, const TArray<FString>& PArray, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);

	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgression1(EsparklogsAnalyticsProgressionStatus Status, const FString& P1);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgression2(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgression3(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgression4(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& P4);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgression5(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& P5);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithReason1(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithReason2(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithReason3(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithReason4(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithReason5(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& P5, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithReasonWithAttrs1(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithReasonWithAttrs2(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithReasonWithAttrs3(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithReasonWithAttrs4(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithReasonWithAttrs5(EsparklogsAnalyticsProgressionStatus Status, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& P5, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);

	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValue1(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValue2(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValue3(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValue4(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& P4);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValue5(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& P5);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValueWithReason1(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValueWithReason2(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValueWithReason3(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValueWithReason4(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValueWithReason5(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& P5, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValueWithReasonWithAttrs1(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValueWithReasonWithAttrs2(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValueWithReasonWithAttrs3(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValueWithReasonWithAttrs4(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionWithValueWithReasonWithAttrs5(EsparklogsAnalyticsProgressionStatus Status, float Value, const FString& P1, const FString& P2, const FString& P3, const FString& P4, const FString& P5, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);

	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionArray(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionArrayWithAttrs(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionArrayWithReason(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionArrayWithReasonWithAttrs(EsparklogsAnalyticsProgressionStatus Status, const TArray<FString>& PArray, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);

	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionArrayWithValue(EsparklogsAnalyticsProgressionStatus Status, float Value, const TArray<FString>& PArray);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionArrayWithValueWithAttrs(EsparklogsAnalyticsProgressionStatus Status, float Value, const TArray<FString>& PArray, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionArrayWithValueWithReason(EsparklogsAnalyticsProgressionStatus Status, float Value, const TArray<FString>& PArray, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordProgressionArrayWithValueWithReasonWithAttrs(EsparklogsAnalyticsProgressionStatus Status, float Value, const TArray<FString>& PArray, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);

	/** Records a design event. EventId is a colon delimited string that forms an event hierarchy of arbitrary depth
	  * (e.g., "quest:trial_master_sword:beginning_floors:start" or "quest:trial_master_sword:middle_floors:finish" or
	  * "tutorial:begin").
	  *
	  * While there is no limit on depth or cardinality of EventId, be smart about how many categories you create based
	  * on your planned analysis.
	  *
	  * Design events may optionally be associated with a numeric value as well.
	  *
	  * If you're in a server process, then you may not have an active session, and instead you want to create an
	  * event that is associated with a session for a given client process. In that case, create a
	  * FSparkLogsAnalyticsSessionDescriptor specifying at least the session ID and user ID and pass to OverrideSessionInfo.
	  *
	  * Returns whether or not the event was created/queued.
	  */
	static bool AddDesign(const TCHAR* EventId, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesign(const TCHAR* EventId, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesign(const TCHAR* EventId, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesign(const TCHAR* EventId, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesign(const TCHAR* EventId, double Value, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesign(const TCHAR* EventId, double Value, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesign(const TCHAR* EventId, double Value, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesign(const TCHAR* EventId, double Value, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesignArray(const TArray<FString>& EventIDParts, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesignArray(const TArray<FString>& EventIDParts, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesignArray(const TArray<FString>& EventIDParts, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesignArray(const TArray<FString>& EventIDParts, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesignArray(const TArray<FString>& EventIDParts, double Value, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesignArray(const TArray<FString>& EventIDParts, double Value, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesignArray(const TArray<FString>& EventIDParts, double Value, const TCHAR* Reason, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddDesignArray(const TArray<FString>& EventIDParts, double Value, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);

	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesign(const FString& EventId);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignWithAttr(const FString& EventId, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignWithReason(const FString& EventId, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignWithReasonWithAttr(const FString& EventId, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);

	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignWithValue(const FString& EventId, float Value);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignWithValueWithAttr(const FString& EventId, float Value, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignWithValueWithReason(const FString& EventId, float Value, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignWithValueWithReasonWithAttr(const FString& EventId, float Value, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);

	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignArrayWithValue(const TArray<FString>& EventIDParts, float Value);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignArrayWithValueWithAttr(const TArray<FString>& EventIDParts, float Value, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignArrayWithValueWithReason(const TArray<FString>& EventIDParts, float Value, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignArrayWithValueWithReasonWithAttr(const TArray<FString>& EventIDParts, float Value, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);

	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignArray(const TArray<FString>& EventIDParts);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignArrayWithAttr(const TArray<FString>& EventIDParts, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignArrayWithReason(const TArray<FString>& EventIDParts, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordDesignArrayWithReasonWithAttr(const TArray<FString>& EventIDParts, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);

	/** Records a log event with key information you want to associate with this analytics session & user.
	 * This could be an error, a key debug event, etc. There are no limits on how many events you can record,
	 * but take care not to ingest too much data (e.g., if you have millions of users, you may want to log
	 * only the first kind of exception per game session, or log crashes, etc.).
	 *
	 * You can optionally specify a reason for this log event.
	 *
	 * If you're in a server process, then you may not have an active session, and instead you want to create an
	 * event that is associated with a session for a given client process. In that case, create a
	 * FSparkLogsAnalyticsSessionDescriptor specifying at least the session ID and user ID and pass to OverrideSessionInfo.
	 *
	 * Returns whether or not the event was created/queued.
	 */
	static bool AddLog(EsparklogsSeverity Severity, const TCHAR* Message, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddLog(EsparklogsSeverity Severity, const TCHAR* Message, TSharedPtr<FJsonObject> CustomAttrs, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddLog(EsparklogsSeverity Severity, const TCHAR* Message, const TCHAR* Reason, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	static bool AddLog(EsparklogsSeverity Severity, const TCHAR* Message, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);

	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordLog(EsparklogsSeverity Severity, const FString& Message);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordLogWithAttr(EsparklogsSeverity Severity, const FString& Message, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordLogWithReason(EsparklogsSeverity Severity, const FString& Message, const FString& Reason);
	UFUNCTION(BlueprintCallable, Category = "SparkLogs")
	static void RecordLogWithReasonWithAttr(EsparklogsSeverity Severity, const FString& Message, const FString& Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs);
};

/**
 * Analytics interface implementation that sends data to the sparklogs module.
 * Typically you would use the UsparklogsAnalytics class rather than this
 * class directly (more covenient, same functionality). See documentation
 * on that class for details.
 */
class SPARKLOGS_API FsparklogsAnalyticsProvider : public IAnalyticsProvider
{
public:	
	static constexpr TCHAR* RootAnalyticsFieldName = TEXT("g_analytics");

	static constexpr TCHAR* StandardFieldEventType = TEXT("type");
	static constexpr TCHAR* EventTypeSessionStart = TEXT("session_start");
	static constexpr TCHAR* EventTypeSessionEnd = TEXT("session_end");
	static constexpr TCHAR* EventTypePurchase = TEXT("purchase");
	static constexpr TCHAR* EventTypeResource = TEXT("resource");
	static constexpr TCHAR* EventTypeProgression = TEXT("progression");
	static constexpr TCHAR* EventTypeDesign = TEXT("design");
	static constexpr TCHAR* EventTypeLog = TEXT("log");

	static constexpr TCHAR* StandardFieldSessionId = TEXT("session_id");
	static constexpr TCHAR* StandardFieldSessionNumber = TEXT("session_num");
	static constexpr TCHAR* StandardFieldSessionStarted = TEXT("session_started");
	static constexpr TCHAR* StandardFieldGameId = TEXT("game_id");
	static constexpr TCHAR* StandardFieldUserId = TEXT("user_id");
	static constexpr TCHAR* StandardFieldPlayerId = TEXT("player_id");
	static constexpr TCHAR* StandardFieldFirstInstalled = TEXT("first_installed");
	static constexpr TCHAR* StandardFieldMeta = TEXT("meta");
	static constexpr TCHAR* StandardFieldCustom = TEXT("custom");

	static constexpr TCHAR* SessionEndFieldSessionEnded = TEXT("session_ended");
	static constexpr TCHAR* SessionEndFieldSessionDurationSecs = TEXT("session_duration_secs");

	static constexpr TCHAR* MetaFieldPlatform = TEXT("platform");
	static constexpr TCHAR* MetaFieldOSVersion = TEXT("os_version");
	static constexpr TCHAR* MetaFieldSDKVersion = TEXT("sdk_version");
	static constexpr TCHAR* MetaFieldEngineVersion = TEXT("engine_version");
	static constexpr TCHAR* MetaFieldBuild = TEXT("build");
	static constexpr TCHAR* MetaFieldDeviceMake = TEXT("device_make");
	static constexpr TCHAR* MetaFieldDeviceModel = TEXT("device_model");
	static constexpr TCHAR* MetaFieldConnectionType = TEXT("connection_type");
	static constexpr TCHAR* MetaFieldGender = TEXT("gender");
	static constexpr TCHAR* MetaFieldLocation = TEXT("location");
	static constexpr TCHAR* MetaFieldAge = TEXT("age");

	static constexpr TCHAR* PurchaseFieldEventId = TEXT("event_id");
	static constexpr TCHAR* PurchaseFieldEventIdParts = TEXT("event_ids");
	static constexpr TCHAR* PurchaseFieldItemCategory = TEXT("item_category");
	static constexpr TCHAR* PurchaseFieldItemId = TEXT("item_id");
	static constexpr TCHAR* PurchaseFieldCurrency = TEXT("currency");
	static constexpr TCHAR* PurchaseFieldAmount = TEXT("amount");
	static constexpr TCHAR* PurchaseFieldReason = TEXT("reason");

	static constexpr TCHAR* ResourceFieldEventId = TEXT("event_id");
	static constexpr TCHAR* ResourceFieldEventIdParts = TEXT("event_ids");
	static constexpr TCHAR* ResourceFieldFlowType = TEXT("flow_type");
	static constexpr TCHAR* ResourceFieldVirtualCurrency = TEXT("virtual_currency");
	static constexpr TCHAR* ResourceFieldItemCategory = TEXT("item_category");
	static constexpr TCHAR* ResourceFieldItemId = TEXT("item_id");
	static constexpr TCHAR* ResourceFieldAmount = TEXT("amount");
	static constexpr TCHAR* ResourceFieldReason = TEXT("reason");

	static constexpr TCHAR* ProgressionFieldEventId = TEXT("event_id");
	static constexpr TCHAR* ProgressionFieldEventIdParts = TEXT("event_ids");
	static constexpr TCHAR* ProgressionFieldStatus = TEXT("status");
	static constexpr TCHAR* ProgressionFieldTiersString = TEXT("tiers");
	static constexpr TCHAR* ProgressionFieldTiersArray = TEXT("tiers_array");
	static constexpr TCHAR* ProgressionFieldTierPrefix = TEXT("tier");
	static constexpr TCHAR* ProgressionFieldAttempt = TEXT("attempt");
	static constexpr TCHAR* ProgressionFieldValue = TEXT("value");
	static constexpr TCHAR* ProgressionFieldReason = TEXT("reason");

	static constexpr TCHAR* DesignFieldEventId = TEXT("event_id");
	static constexpr TCHAR* DesignFieldEventIdParts = TEXT("event_ids");
	static constexpr TCHAR* DesignFieldValue = TEXT("value");
	static constexpr TCHAR* DesignFieldReason = TEXT("reason");

	static constexpr TCHAR* LogFieldSeverity = TEXT("severity");
	static constexpr TCHAR* LogFieldReason = TEXT("reason");

	static constexpr TCHAR* MessageHeader = TEXT("GAME_ENGINE_ANALYTICS");
	static constexpr TCHAR* ItemSeparator = TEXT(":");

	static constexpr TCHAR* RecordEventGenericEventId = TEXT("generic");
	static constexpr TCHAR* RecordItemPurchasePerItemCost = TEXT("per_item_cost");
	static constexpr TCHAR* RecordItemPurchaseItemQuantity = TEXT("item_quantity");
	static constexpr TCHAR* RecordItemPurchaseItemCategory = TEXT("item_purchase");
	static constexpr TCHAR* RecordCurrencyPurchasePaymentProvider = TEXT("payment_provider");
	static constexpr TCHAR* RecordCurrencyPurchasePurchaseItemCategory = TEXT("game_currency");
	static constexpr TCHAR* RecordCurrencyPurchaseResourceItemCategory = TEXT("currency_purchase");
	static constexpr TCHAR* RecordCurrencyGivenItemCategory = TEXT("currency_given");
	static constexpr TCHAR* const RecordProgressDelimiters[2] = { TEXT(":"), TEXT(".") };

public:
	FsparklogsAnalyticsProvider(TSharedRef<FsparklogsSettings> InSettings);
	virtual ~FsparklogsAnalyticsProvider();

	virtual bool CreateAnalyticsEventPurchase(const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* RealCurrencyCode, double Amount, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage=true, const TCHAR* ExtraMessage=nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession=nullptr);
	virtual bool CreateAnalyticsEventPurchase(const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* RealCurrencyCode, double Amount, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, bool IncludeDefaultMessage=true, const TCHAR* ExtraMessage=nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession=nullptr);

	virtual bool CreateAnalyticsEventResource(EsparklogsAnalyticsFlowType FlowType, double Amount, const TCHAR* VirtualCurrency, const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	virtual bool CreateAnalyticsEventResource(EsparklogsAnalyticsFlowType FlowType, double Amount, const TCHAR* VirtualCurrency, const TCHAR* ItemCategory, const TCHAR* ItemId, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);

	virtual bool CreateAnalyticsEventProgression(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	virtual bool CreateAnalyticsEventProgression(EsparklogsAnalyticsProgressionStatus Status, double Value, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	virtual bool CreateAnalyticsEventProgression(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	virtual bool CreateAnalyticsEventProgression(EsparklogsAnalyticsProgressionStatus Status, const TCHAR* P1, const TCHAR* P2, const TCHAR* P3, const TCHAR* P4, const TCHAR* P5, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	virtual bool CreateAnalyticsEventProgression(EsparklogsAnalyticsProgressionStatus Status, double* Value, const TArray<FString>& PArray, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);

	virtual bool CreateAnalyticsEventDesign(const TCHAR* EventId, double Value, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	virtual bool CreateAnalyticsEventDesign(const TCHAR* EventId, double Value, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	virtual bool CreateAnalyticsEventDesign(const TCHAR* EventId, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	virtual bool CreateAnalyticsEventDesign(const TCHAR* EventId, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	virtual bool CreateAnalyticsEventDesign(const TArray<FString>& EventIDParts, double* Value, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, bool IncludeDefaultMessage = true, const TCHAR* ExtraMessage = nullptr, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);

	virtual bool CreateAnalyticsEventLog(EsparklogsSeverity Severity, const TCHAR* Message, const TCHAR* Reason, TSharedPtr<FJsonObject> CustomAttrs, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);
	virtual bool CreateAnalyticsEventLog(EsparklogsSeverity Severity, const TCHAR* Message, const TCHAR* Reason, const TArray<FsparklogsAnalyticsAttribute>& CustomAttrs, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession = nullptr);

	//~ Begin IAnalyticsProvider Interface

	/** Starts a session (if one has already started, does nothing and treats as success). */
	virtual bool StartSession(const TArray<FAnalyticsEventAttribute>& Attributes) override;

	/** Ends a session if any is active (if one is not active, does nothing) */
	virtual void EndSession() override;
	virtual FString GetSessionID() const override;

	/** It's recommended NOT to use this and instead pass OverrideSession arguments instead to other functions. */
	virtual bool SetSessionID(const FString& InSessionID) override;
	virtual void FlushEvents() override;

	/** It's recommended NOT to use this and instead pass OverrideSession arguments instead to other functions. */
	virtual void SetUserID(const FString& InUserID) override;

	virtual FString GetUserID() const override;
	virtual void SetBuildInfo(const FString& InBuildInfo) override;
	virtual void SetGender(const FString& InGender) override;
	virtual void SetLocation(const FString& InLocation) override;
	virtual void SetAge(const int32 InAge) override;
	virtual void RecordEvent(const FString& EventName, const TArray<FAnalyticsEventAttribute>& Attributes) override;
	virtual void RecordItemPurchase(const FString& ItemId, const FString& Currency, int PerItemCost, int ItemQuantity) override;
	virtual void RecordItemPurchase(const FString& ItemId, int ItemQuantity, const TArray<FAnalyticsEventAttribute>& EventAttrs) override;
	virtual void RecordCurrencyPurchase(const FString& GameCurrencyType, int GameCurrencyAmount, const FString& RealCurrencyType, float RealMoneyCost, const FString& PaymentProvider) override;
	virtual void RecordCurrencyPurchase(const FString& GameCurrencyType, int GameCurrencyAmount, const TArray<FAnalyticsEventAttribute>& EventAttrs) override;
	virtual void RecordCurrencyGiven(const FString& GameCurrencyType, int GameCurrencyAmount, const TArray<FAnalyticsEventAttribute>& EventAttrs) override;
	virtual void RecordError(const FString& Error, const TArray<FAnalyticsEventAttribute>& EventAttrs) override;
	virtual void RecordProgress(const FString& ProgressType, const FString& ProgressHierarchy) override;
	virtual void RecordProgress(const FString& ProgressType, const FString& ProgressHierarchy, const TArray<FAnalyticsEventAttribute>& EventAttrs) override;
	virtual void RecordProgress(const FString& ProgressType, const TArray<FString>& ProgressHierarchy, const TArray<FAnalyticsEventAttribute>& EventAttrs) override;

#if (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
	virtual FAnalyticsEventAttribute GetDefaultEventAttribute(int AttributeIndex) const override;
	virtual int32 GetDefaultEventAttributeCount() const override;
	virtual TArray<FAnalyticsEventAttribute> GetDefaultEventAttributesSafe() const override;
	virtual void SetDefaultEventAttributes(TArray<FAnalyticsEventAttribute>&& Attributes) override;
#else
	virtual FAnalyticsEventAttribute GetDefaultEventAttribute(int AttributeIndex) const;
	virtual int32 GetDefaultEventAttributeCount() const;
	virtual TArray<FAnalyticsEventAttribute> GetDefaultEventAttributesSafe() const;
	virtual void SetDefaultEventAttributes(TArray<FAnalyticsEventAttribute>&& Attributes);
#endif
	//~ End IAnalyticsProvider Interface

	// Automatically cleanup any active session, except for server launch configurations where we only send session data if they explicitly use StartSession and EndSession.
	virtual void AutoCleanupSession();

	/** Thread-safe. Gets a COPY of the information needed to record an analytics event. */
	void GetAnalyticsEventData(FString& OutSessionID, FDateTime& OutSessionStarted, int& OutSessionNumber, TSharedPtr<FJsonObject>& OutMetaAttributes) const;

	/** Thread-safe. Sets the given field in meta attributes to the given value. If the value is an object you should NEVER mutate it after storing it into the meta attributes. */
	void SetMetaAttribute(const FString& Field, const TSharedPtr<FJsonValue> Value);
	
	/** Thread-safe. Gets the number of active sessions we've had on this device since install (starting from 1), including the current session. */
	int GetSessionNumber();
	/** Thread-safe. Do not use this, use OverrideSession arguments instead. */
	void SetSessionNumber(int N);

	/** Thread-safe. Gets the start time of the session. */
	FDateTime GetSessionStarted();
	/** Thread-safe. Do not use this, use OverrideSession arguments instead. */
	void SetSessionStarted(FDateTime DT);

	/** Thread-safe. Adds all standard fields to the given analytics logical event so it can be queued as a raw event. It modifies Object in-place.
	  * This will automatically add meta attributes to the raw analytics event data. Normally you should call higher-level Analytics* methods that
	  * generate higher-level events that follow certain structures.
	  * 
	  * If a session is NOT active or if there is no game ID or user ID, it will forcefully set Object to nullptr (so the event cannot be sent) unless you specify OverrideSession.
	  * If OverrideSession is not nullptr/empty then you can override the session ID/number/start/user ID (useful on the server).
	  */
	void FinalizeAnalyticsEvent(const TCHAR* EventType, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession, TSharedPtr<FJsonObject>& Object);

public:
	static void AddAnalyticsEventAttributeToJsonObject(const TSharedPtr<FJsonObject> Object, const FAnalyticsEventAttribute& Attr);
	static void AddAnalyticsEventAttributesToJsonObject(const TSharedPtr<FJsonObject> Object, const TArray<FAnalyticsEventAttribute>& EventAttrs);
	static void AddAnalyticsEventAttributeToJsonObject(const TSharedPtr<FJsonObject> Object, const FsparklogsAnalyticsAttribute& Attr);
	static void AddAnalyticsEventAttributesToJsonObject(const TSharedPtr<FJsonObject> Object, const TArray<FsparklogsAnalyticsAttribute>& EventAttrs);
	static FAnalyticsEventAttribute ConvertJsonValueToEventAttribute(const FString& Key, TSharedPtr<FJsonValue> Value);
	// Returns a JSON array value that contains the strings from the given array. Ignores empty string values in the array.
	static TSharedRef<FJsonValueArray> ConvertNonEmptyStringArrayToJSON(const TArray<FString>& A);

protected:
	TSharedRef<FsparklogsSettings> Settings;

	// Protects access to any of data below this declaration.
	mutable FCriticalSection DataCriticalSection;
	// If non-empty, the ID of the session that is currently active.
	FString CurrentSessionID;
	// Time when session started
	FDateTime SessionStarted;
	// The number of active sessions we've had on this device since install (starting from 1), including the current session. Incremented when the session starts.
	int SessionNumber;
	// The attributes we want to include with EVERY analytics event.
	TSharedRef<FJsonObject> MetaAttributes;
	// The flattened tiers (e.g., tier1:tier2,tier3) of the progression events that are currently in progress.
	TSet<FString> InProgressProgression;

	// Not thread-safe (hold lock if needed). Setup defaults for meta attributes.
	void SetupDefaultMetaAttributes();

	// Like FinalizeAnalyticsEvent but will assume the critical section is ALREADY LOCKED!
	void InternalFinalizeAnalyticsEvent(const TCHAR* EventType, const FSparkLogsAnalyticsSessionDescriptor* OverrideSession, TSharedPtr<FJsonObject>& Object);

	// Forms the final message that should be used given a default message, an extra message, and whether or not the default should be included.
	FString CalculateFinalMessage(const FString& DefaultMessage, bool IncludeDefaultMessage, const TCHAR* ExtraMessage);

	// Returns true if the progression event is valid
	bool ValidateProgressionEvent(const TArray<FString>& PArray);
	// Returns an EventID for a given (valid) progression event. Returns an empty string for an invalid progression event.
	FString GetProgressionEventID(const TArray<FString>& PArray);
};

/**
* Main plugin module. Reads settings and handles startup/shutdown.
*/
class SPARKLOGS_API FsparklogsModule : public IAnalyticsProviderModule
{
public:
	static constexpr const TCHAR* OverrideAutoExtractDisabled = TEXT("__autoextract_disabled");

public:
	/**
	 * Returns the singleton instance of this module, loading it on demand if needed.
	 * Be careful calling this during shutdown when the module may already be unloaded!
	 */
	static inline FsparklogsModule& GetModule()
	{
		return FModuleManager::LoadModuleChecked<FsparklogsModule>(FName(ITL_PLUGIN_MODULE_NAME));
	}

	/** Returns whether or not the module is currently loaded. */
	static inline bool IsModuleLoaded()
	{
		return FModuleManager::Get().IsModuleLoaded(FName(ITL_PLUGIN_MODULE_NAME));
	}

public:
	FsparklogsModule();

	//~ Begin IModuleInterface Interface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface Interface

	//~ Begin IAnalyticsProviderModule Interface
	virtual TSharedPtr<IAnalyticsProvider> CreateAnalyticsProvider(const FAnalyticsProviderConfigurationDelegate& GetConfigValue) const override;
	//~ End IAnalyticsProviderModule Interface

	/** Returns the SparkLogs analytics provider singleton (will always be valid). */
	static TSharedPtr<FsparklogsAnalyticsProvider> GetAnalyticsProvider();

	/** Thread-safe. Queues a raw analytics event of the given type for sending.
	  * Prepare a raw analytics event using FsparklogsAnalyticsProvider::FinalizeAnalyticsEvent.
	  * 
	  * Normally you would interact with the analytics provider (see GetAnalyticsProvider)
	  * for a higher level API that will produce and queue raw analytics events in a standard format.
	  * If analytics is not enabled, returns false. Returns true if the data was queued. */
	virtual bool AddRawAnalyticsEvent(TSharedPtr<FJsonObject> RawAnalyticsData, const TCHAR* LogMessage, TSharedPtr<FJsonObject> CustomRootFields, bool ForceDisableAutoExtract);

	/** Returns the random game_instance_id used for this run of the engine.
	  * There may be multiple game analytics sessions during a single game instance.
	  * This is available after the module has started (even before the shipping
	  * engine is started.*/
	FString GetGameInstanceID();

	/**
	 * Starts the log/event shipping engine if it has not yet started.
	 * 
	 * You can override the analytics user ID (normally we generate automatically) if desired
	 * by specifying a non-null, non-empty value. Any custom analytics user ID should be globally
	 * unique and make sure to respect any privacy rules for your app.
	 * 
	 * You can override the agent ID and/or agent auth token by specifying non-empty values.
	 *
	 * If you are sending data to your own HTTP endpoint URI instead of the SparkLogs cloud,
	 * then you can choose to override the destination HTTP endpoint and/or override the
	 * authentication header value directly.
	 * 
	 * You can also optionally override the computer name that will be used in the metadata
	 * sent with all log agents -- the default is to use FPlatformProcess::ComputerName().
	 * (If NULL or empty values are passed for override strings, then the default values will be used, etc.)
	 * 
	 * You can optionally pass additional custom attributes that will be added to all shipped log/analytics events.
	 * 
	 * This will still only activate if a random roll of the dice passed the "ActivationPercentage" check, or pass
	 * AlwaysStart as true to force the engine to start regardless of this setting.
	 *
	 * Returns true if the shipping engine was activated (may be false if diceroll + ActivationPercentage caused it to not start).
	 */
	bool StartShippingEngine(const TCHAR* OverrideAnalyticsUserID, const TCHAR* OverrideAgentID, const TCHAR* OverrideAgentAuthToken, const TCHAR* OverrideHTTPEndpointURI, const TCHAR* OverrideHttpAuthorizationHeaderValue, const TCHAR* OverrideComputerName, TMap<FString, FString>* AdditionalAttributes, bool AlwaysStart);

	/** Stops the log/event shipping engine. Any active analytics session (if any) will end. It will not start again unless StartShippingEngine is manually called. */
	void StopShippingEngine();

	/** Triggers an immediate flush of queued log/analytics events to attempt to be sent to the cloud. Does not wait for this to finish. */
	void Flush();

protected:
	/** Called by the engine after it has fully initialized. */
	void OnPostEngineInit();
	/** Called by the engine as part of its exit process. */
	void OnEngineExit();

private:
	/** Singleton analytics provider */
	static TSharedPtr<FsparklogsAnalyticsProvider> AnalyticsProvider;

	FString GameInstanceID;
	bool EngineActive;
	TSharedRef<FsparklogsSettings> Settings;
	TSharedPtr<FsparklogsReadAndStreamToCloud, ESPMode::ThreadSafe> CloudStreamer;
	TUniquePtr<FsparklogsStressGenerator> StressGenerator;
	/** The payload processor that sends data to the cloud */
	TSharedPtr<FsparklogsWriteHTTPPayloadProcessor, ESPMode::ThreadSafe> CloudPayloadProcessor;

	void RegisterSettings();
	void UnregisterSettings();
};
