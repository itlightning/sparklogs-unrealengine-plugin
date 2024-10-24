// Copyright (C) 2024 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "HAL/Runnable.h"

#define ITL_CONFIG_SECTION_NAME TEXT("ITLightning")

DECLARE_LOG_CATEGORY_EXTERN(LogPluginITLightning, Log, All);

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
	static constexpr TCHAR* PluginStateSection = TEXT("PluginState");

	static constexpr int DefaultBytesPerRequest = 1024 * 1024;
	static constexpr int MinBytesPerRequest = 1024 * 16;
	static constexpr int MaxBytesPerRequest = 1024 * 1024 * 4;
	static constexpr double DefaultProcessIntervalSec = 1.0;
	static constexpr double MinProcessIntervalSec = 0.2;
	static constexpr double DefaultRetryIntervalSec = 10.0;
	static constexpr double MinRetryIntervalSec = 1.0;
	static constexpr double WaitForFlushToCloudOnShutdown = 15.0;
	static constexpr bool DefaultIncludeCommonMetadata = true;

	/** ID of the agent when pushing logs to the cloud */
	FString AgentID;
	/** Auth token associated with the agent when pushing logs to the cloud */
	FString AuthToken;
	/** What percent of the time to activate this plugin. 0.0 to 100.0. Defaults to 100%. Useful for incrementally rolling out the plugin. */
	double ActivationPercent;
	/** Desired maximum bytes to read and process at one time (one "chunk"). */
	int32 BytesPerRequest;
	/** Desired seconds between attempts to read and process a chunk. */
	double ProcessIntervalSec;
	/** The amount of time to wait after a failed request before retrying. */
	double RetryIntervalSec;
	/** Whether or not to include common metadata in each log event. */
	bool IncludeCommonMetadata;

	FitlightningSettings();

	/** Loads the settings from the game engine INI section appropriate for this runtime mode (editor, client, server, etc). */
	void LoadSettings();

protected:
	/** Enforces constraints upon any loaded setting values. */
	void EnforceConstraints();
};

/**
 * An interface that takes a JSON log payload from the WORKER thread of the streamer, and processes it.
 */
class IitlightningPayloadProcessor
{
public:
	virtual ~IitlightningPayloadProcessor() = default;
	/** Processes the JSON payload, and returns true on success or false on failure. */
	virtual bool ProcessPayload(const uint8* JSONPayloadInUTF8, int PayloadLen) = 0;
};

/** A payload processor that writes the data to a local file (for DEBUG purposes only). */
class FitlightningWriteNDJSONPayloadProcessor : public IitlightningPayloadProcessor
{
protected:
	FString OutputFilePath;
public:
	FitlightningWriteNDJSONPayloadProcessor(FString InOutputFilePath);
	virtual bool ProcessPayload(const uint8* JSONPayloadInUTF8, int PayloadLen) override;
};

using TITLJSONStringBuilder = TAnsiStringBuilder<4 * 1024>;

/**
* On a background thread, reads data from a logfile on disk and streams to the cloud.
*/
class FitlightningReadAndStreamToCloud : public FRunnable
{
protected:
	static constexpr TCHAR* ProgressMarkerValue = TEXT("ShippedLogOffset");

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
	virtual bool FlushAndWait(int N, bool ClearRetryTimer, bool InitiateStop, double TimeoutSec, bool& OutLastFlushProcessedEverything);

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

private:

	bool LoggingActive;
	TSharedRef<FitlightningSettings> Settings;
	TUniquePtr<FitlightningReadAndStreamToCloud> CloudStreamer;
};
