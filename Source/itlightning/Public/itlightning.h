// Copyright (C) 2024 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Object.h"
#include "Modules/ModuleManager.h"
#include "HAL/Runnable.h"
#include "itlightning.generated.h"

#define ITL_CONFIG_SECTION_NAME TEXT("/Script/itlightning.ITLightningRuntimeSettings")

DECLARE_LOG_CATEGORY_EXTERN(LogPluginITLightning, Log, All);

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
FString ITLConvertUTF8(const void* Data, int Len);

/**
 * Manages plugin settings.
 */
class FitlightningSettings
{
public:
	static const TCHAR* PluginStateSection;

	static constexpr double DefaultRequestTimeoutSecs = 30;
	static constexpr double MinRequestTimeoutSecs = 5;
	static constexpr double DefaultActivationPercentage = 100.0;
	static constexpr int DefaultBytesPerRequest = 2 * 1024 * 1024;
	static constexpr int MinBytesPerRequest = 1024 * 128;
	static constexpr int MaxBytesPerRequest = 1024 * 1024 * 4;
	static constexpr double DefaultProcessingIntervalSecs = 2.0;
	static constexpr double MinProcessingIntervalSecs = 0.5;
	static constexpr double DefaultRetryIntervalSecs = 20.0;
	static constexpr double MinRetryIntervalSecs = 3.0;
	static constexpr double WaitForFlushToCloudOnShutdown = 15.0;
	static constexpr bool DefaultIncludeCommonMetadata = true;
	static constexpr bool DefaultDebugLogRequests = false;

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
	/** What percent of the time to activate this plugin. 0.0 to 100.0. Defaults to 100%. Useful for incrementally rolling out the plugin. */
	double ActivationPercentage;
	/** Desired maximum bytes to read and process at one time (one "chunk"). */
	int32 BytesPerRequest;
	/** Desired seconds between attempts to read and process a chunk. */
	double ProcessingIntervalSecs;
	/** The amount of time to wait after a failed request before retrying. */
	double RetryIntervalSecs;
	/** Whether or not to include common metadata in each log event. */
	bool IncludeCommonMetadata;
	/** Whether or not to log requests */
	bool DebugLogRequests;

	FitlightningSettings();

	/** Loads the settings from the game engine INI section appropriate for this launch configuration (editor, client, server, etc). */
	void LoadSettings();

	/** Gets the effective HTTP endpoint URI (either using the HttpEndpointURI if configured, or the CloudRegion). Returns empty if not configured. */
	FString GetEffectiveHttpEndpointURI();

protected:
	/** Enforces constraints upon any loaded setting values. */
	void EnforceConstraints();
};

/**
 * Settings for the plugin that will be managed and edited by Unreal Engine
 */
UCLASS(config=Engine, DefaultConfig)
class ITLIGHTNING_API UITLightningRuntimeSettings : public UObject
{
	GENERATED_BODY()

public:
	// ------------------------------------------ SERVER LAUNCH CONFIGURATION SETTINGS
	
	// Set to 'us' or 'eu' based on what your IT Lightning workspace is provisioned to use.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Cloud Region")
	FString ServerCloudRegion;

	// For authentication: ID of the cloud agent that will receive the ingested log data.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Agent ID")
	FString ServerAgentID;

	// For authentication: Auth token associated with the cloud agent receiving the log data.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Agent Auth Token")
	FString ServerAgentAuthToken;

	// What percent of the time to activate this plugin when the engine starts. 0.0 to 100.0. Defaults to 100%. Useful for incrementally rolling out the plugin.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Activation Percentage")
	float ServerActivationPercentage = FitlightningSettings::DefaultActivationPercentage;

	// HTTP request timeout in seconds.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Server Launch Configuration", DisplayName = "Request Timeout in Seconds")
	float ServerRequestTimeoutSecs = FitlightningSettings::DefaultRequestTimeoutSecs;

	// ------------------------------------------ EDITOR LAUNCH CONFIGURATION SETTINGS

	// Set to 'us' or 'eu' based on what your IT Lightning workspace is provisioned to use. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Cloud Region")
	FString EditorCloudRegion;

	// For authentication: ID of the cloud agent that will receive the ingested log data. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName="Agent ID")
	FString EditorAgentID;

	// For authentication: Auth token associated with the cloud agent receiving the log data. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName="Agent Auth Token")
	FString EditorAgentAuthToken;

	// What percent of the time to activate this plugin when the engine starts. 0.0 to 100.0. Defaults to 100%. Useful for incrementally rolling out the plugin. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName="Activation Percentage")
	float EditorActivationPercentage = FitlightningSettings::DefaultActivationPercentage;

	// HTTP request timeout in seconds. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Request Timeout in Seconds")
	float EditorRequestTimeoutSecs = FitlightningSettings::DefaultRequestTimeoutSecs;

	// ------------------------------------------ CLIENT LAUNCH CONFIGURATION SETTINGS

	// Set to 'us' or 'eu' based on what your IT Lightning workspace is provisioned to use.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Cloud Region")
	FString ClientCloudRegion;

	// For authentication: ID of the cloud agent that will receive the ingested log data.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Agent ID")
	FString ClientAgentID;

	// For authentication: Auth token associated with the cloud agent receiving the log data.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Agent Auth Token")
	FString ClientAgentAuthToken;

	// What percent of the time to activate this plugin when the engine starts. 0.0 to 100.0. Defaults to 100%. Useful for incrementally rolling out the plugin.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Activation Percentage")
	float ClientActivationPercentage = FitlightningSettings::DefaultActivationPercentage;

	// HTTP request timeout in seconds.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Settings In Client Launch Configuration", DisplayName = "Request Timeout in Seconds")
	float ClientRequestTimeoutSecs = FitlightningSettings::DefaultRequestTimeoutSecs;

	// ------------------------------------------ SERVER LAUNCH CONFIGURATION ADVANCED SETTINGS

	// Target bytes to read and process at one time (one "chunk").
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "Bytes Per Request")
	int32 ServerBytesPerRequest = FitlightningSettings::DefaultBytesPerRequest;

	// Target seconds between attempts to read and process a chunk.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "Processing Interval in Seconds")
	float ServerProcessingIntervalSecs = FitlightningSettings::DefaultProcessingIntervalSecs;

	// The amount of time to wait after a failed request before retrying.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "Retry Interval in Seconds")
	float ServerRetryIntervalSecs = FitlightningSettings::DefaultRetryIntervalSecs;

	// Whether or not to include common metadata (hostname, game name) in each log event.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "Include Common Metadata")
	bool ServerIncludeCommonMetadata = FitlightningSettings::DefaultIncludeCommonMetadata;

	// Normally leave blank and set CloudRegion. Overrides the URI of the endpoint to push log payloads to, e.g., https://ingest-<REGION>.engine.itlightning.app/ingest/v1
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "Override HTTP Endpoint URI")
	FString ServerHTTPEndpointURI;

	// For Debugging: Whether or not to log requests.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Server Launch Configuration", DisplayName = "DEBUG: Log All HTTP Request")
	bool ServerDebugLogRequests = FitlightningSettings::DefaultDebugLogRequests;

	// ------------------------------------------ EDITOR LAUNCH CONFIGURATION ADVANCED SETTINGS

	// Target bytes to read and process at one time (one "chunk"). [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Bytes Per Request")
	int32 EditorBytesPerRequest = FitlightningSettings::DefaultBytesPerRequest;

	// Target seconds between attempts to read and process a chunk. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Processing Interval in Seconds")
	float EditorProcessingIntervalSecs = FitlightningSettings::DefaultProcessingIntervalSecs;

	// The amount of time to wait after a failed request before retrying. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Retry Interval in Seconds")
	float EditorRetryIntervalSecs = FitlightningSettings::DefaultRetryIntervalSecs;

	// Whether or not to include common metadata (hostname, game name) in each log event. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Include Common Metadata")
	bool EditorIncludeCommonMetadata = FitlightningSettings::DefaultIncludeCommonMetadata;

	// Normally leave blank and set CloudRegion. Overrides the URI of the endpoint to push log payloads to, e.g., https://ingest-<REGION>.engine.itlightning.app/ingest/v1 [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "Override HTTP Endpoint URI")
	FString EditorHTTPEndpointURI;

	// For Debugging: Whether or not to log requests. [EDITOR RESTART REQUIRED]
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Editor Launch Configuration", Meta = (ConfigRestartRequired = true), DisplayName = "DEBUG: Log All HTTP Request")
	bool EditorDebugLogRequests = FitlightningSettings::DefaultDebugLogRequests;

	// ------------------------------------------ CLIENT LAUNCH CONFIGURATION ADVANCED SETTINGS

	// Target bytes to read and process at one time (one "chunk").
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "Bytes Per Request")
	int32 ClientBytesPerRequest = FitlightningSettings::DefaultBytesPerRequest;

	// Target seconds between attempts to read and process a chunk.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "Processing Interval in Seconds")
	float ClientProcessingIntervalSecs = FitlightningSettings::DefaultProcessingIntervalSecs;

	// The amount of time to wait after a failed request before retrying.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "Retry Interval in Seconds")
	float ClientRetryIntervalSecs = FitlightningSettings::DefaultRetryIntervalSecs;

	// Whether or not to include common metadata (hostname, game name) in each log event.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "Include Common Metadata")
	bool ClientIncludeCommonMetadata = FitlightningSettings::DefaultIncludeCommonMetadata;

	// Normally leave blank and set CloudRegion. Overrides the URI of the endpoint to push log payloads to, e.g., https://ingest-<REGION>.engine.itlightning.app/ingest/v1
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "Override HTTP Endpoint URI")
	FString ClientHTTPEndpointURI;

	// For Debugging: Whether or not to log requests.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Advanced Settings In Client Launch Configuration", DisplayName = "DEBUG: Log All HTTP Request")
	bool ClientDebugLogRequests = FitlightningSettings::DefaultDebugLogRequests;
};

class FitlightningReadAndStreamToCloud;

/**
 * An interface that takes a JSON log payload from the WORKER thread of the streamer, and processes it.
 */
class IitlightningPayloadProcessor
{
public:
	virtual ~IitlightningPayloadProcessor() = default;
	/** Processes the JSON payload, and returns true on success or false on failure. */
	virtual bool ProcessPayload(const uint8* JSONPayloadInUTF8, int PayloadLen, FitlightningReadAndStreamToCloud* Streamer) = 0;
};

/** A payload processor that writes the data to a local file (for DEBUG purposes only). */
class FitlightningWriteNDJSONPayloadProcessor : public IitlightningPayloadProcessor
{
protected:
	FString OutputFilePath;
public:
	FitlightningWriteNDJSONPayloadProcessor(FString InOutputFilePath);
	virtual bool ProcessPayload(const uint8* JSONPayloadInUTF8, int PayloadLen, FitlightningReadAndStreamToCloud* Streamer) override;
};

/** A payload processor that synchronously POSTs the data to an HTTP(S) endpoint. */
class FitlightningWriteHTTPPayloadProcessor : public IitlightningPayloadProcessor
{
protected:
	FString EndpointURI;
	FString AuthorizationHeader;
	FThreadSafeCounter TimeoutMillisec;
	TArray<uint8> InternalBuffer;
	bool LogRequests;
public:
	FitlightningWriteHTTPPayloadProcessor(const TCHAR* InEndpointURI, const TCHAR* InAuthorizationHeader, double InTimeoutSecs, bool InLogRequests);
	virtual bool ProcessPayload(const uint8* JSONPayloadInUTF8, int PayloadLen, FitlightningReadAndStreamToCloud* Streamer) override;
	void SetTimeoutSecs(double InTimeoutSecs);
};

using TITLJSONStringBuilder = TAnsiStringBuilder<4 * 1024>;

/**
* On a background thread, reads data from a logfile on disk and streams to the cloud.
*/
class FitlightningReadAndStreamToCloud : public FRunnable
{
protected:
	static const TCHAR* ProgressMarkerValue;

	TSharedRef<FitlightningSettings> Settings;
	TSharedRef<IitlightningPayloadProcessor> PayloadProcessor;
	FString ProgressMarkerPath;
	FString SourceLogFile;
	int MaxLineLength;

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
	/** [WORKER] The offset where we next need to start processing data in the logfile. */
	int64 WorkerShippedLogOffset;
	/** [WORKER] If non-zero, the minimum time when we can attempt to flush to cloud again automatically. Useful to wait longer to retry after a failure. */
	double WorkerMinNextFlushPlatformTime;
	/** Whether or not the next flush platform time is because of a failure. */
	FThreadSafeBool WorkerLastFlushFailed;

	virtual void ComputeCommonEventJSON();

public:

	FitlightningReadAndStreamToCloud(const TCHAR* SourceLogFile, TSharedRef<FitlightningSettings> InSettings, TSharedRef<IitlightningPayloadProcessor> InPayloadProcessor, int InMaxLineLength);
	~FitlightningReadAndStreamToCloud();

	//~ Begin FRunnable Interface
	virtual bool Init();
	virtual uint32 Run();
	virtual void Stop();
	//~ End FRunnable Interface

	/** Initiate Flush up to N times, optionally clear retry timer to try again immediately, optionally initiate Stop, and wait up through a timeout for each flush to complete. Returns false on timeout or if the flush failed. */
	virtual bool FlushAndWait(int N, bool ClearRetryTimer, bool InitiateStop, bool OnMainGameThread, double TimeoutSec, bool& OutLastFlushProcessedEverything);

	/** Read the progress marker. Returns false on failure. */
	virtual bool ReadProgressMarker(int64& OutMarker);
	/** Writes the progress marker. Returns false on failure. */
	virtual bool WriteProgressMarker(int64 InMarker);
	/** Delete the progress marker */
	virtual void DeleteProgressMarker();

protected:
	/** [WORKER] Build the JSON payload from as much of the data in WorkerBuffer as possible, up to NumToRead bytes. Sets OutCapturedOffset to the number of bytes captured into the payload. Returns false on failure. Do not call directly. */
	virtual bool WorkerBuildNextPayload(int NumToRead, int& OutCapturedOffset, int& OutNumCapturedLines);
	/** [WORKER] Does the actual work for the flush operation, returns true on success. Does not update progress marker or thread state. Do not call directly. */
	virtual bool WorkerInternalDoFlush(int64& OutNewShippedLogOffset, bool& OutFlushProcessedEverything);
	/** [WORKER] Attempts to flush any newly available logs to the cloud. Response for updating flush op counters, LastFlushProcessedEverything, and MinNextFlushPlatformTime state. Returns false on failure. Only call from worker thread. */
	virtual bool WorkerDoFlush();
};

/**
* Main plugin module. Reads settings and handles startup/shutdown.
*/
class FitlightningModule : public IModuleInterface
{

public:
	FitlightningModule();

	//~ Begin IModuleInterface Interface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface Interface

	/** Stop the log shipping engine. It will not start again. */
	void StopShippingEngine();

protected:
	/** Called by the engine after it has fully initialized. */
	void OnPostEngineInit();
	/** Called by the engine as part of its exit process. */
	void OnEngineExit();

private:

	bool LoggingActive;
	TSharedRef<FitlightningSettings> Settings;
	TUniquePtr<FitlightningReadAndStreamToCloud> CloudStreamer;
	/** The payload processor that sends data to the cloud */
	TSharedPtr<FitlightningWriteHTTPPayloadProcessor> CloudPayloadProcessor;

	void RegisterSettings();
	void UnregisterSettings();
};
