# SparkLogs Plugin: Sending Logs to a Local Elasticsearch + Kibana Cluster

This guide explains how to use the SparkLogs plugin to send logs to a local Elasticsearch and Kibana cluster using Docker Compose.

Elasticsearch is a popular open source engine for indexing and querying documents. Kibana is an open source user interface for Elasticsearch.
This example preconfigures Elasticsearch with the appropriate index template to receive the log data, and also preconfigures
Kibana with a data view for visualizing the log data ingested into the Elasticsearch cluster.

If you want to easily collaborate over logs with your entire team, consider using the
plugin with the [SparkLogs Cloud](https://sparklogs.com/). Configuration is easy,
just specify the cloud region for your account in the plugin settings, as well as the
agent ID and auth token from your account. Logs will then flow to your cloud account.

## Prerequisites

You will need about 1.5 GB of RAM for the elasticsearch/kibana Docker containers.

1. Install [Docker](https://www.docker.com/)
2. Ensure the SparkLogs plugin is installed in your Unreal Engine project.

## Steps to Set Up

### 1. Start Elasticsearch and Kibana
- Navigate to the `local-elasticsearch-kibana` directory.
- Double click start.bat and wait for all containers to start.

### 2. Configure the Plugin
- In the Unreal Engine editor, go to Edit, Project Settings, and go to the section for the SparkLogs plugin.
- For the appropriate launch configuration (e.g., Editor), set the `Custom HTTP Endpoint URI` to `http://localhost:9200` (restart editor if required)
- Alternatively, open the `DefaultEngine.ini` configuration file in your Unreal Engine project and edit settings appropriately. For example:

```ini
[/Script/sparklogs.SparkLogsRuntimeSettings]
EditorHTTPEndpointURI="http://localhost:9880/"
```

### 3. Send Logs
- In your project, use the `UE_LOG` log macro like normal.
- The plugin will ship logs to the local Elasticsearch cluster through the local vector.dev container.

### 4. Visualize Logs in Kibana
- Open Kibana's Discover area at `http://localhost:5601/app/discover`.
- The Docker compose script will have already created a data view for the vector log indexes, so you should be able to browse and search log data right away.

## Stopping the Cluster
You can stop the cluster by running `stop.bat` or completely purge all data by running `stop-and-purge.bat`

## Troubleshooting
- Ensure Docker services are running if you encounter connection issues.
- Check the Docker console logs for the appropriate container.
