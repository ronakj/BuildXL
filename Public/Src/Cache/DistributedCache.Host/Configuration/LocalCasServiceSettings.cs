﻿// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System.Runtime.Serialization;
using BuildXL.Cache.ContentStore.Grpc;
#nullable disable

namespace BuildXL.Cache.Host.Configuration
{
    [DataContract]
    public class LocalCasServiceSettings
    {
        /// <nodoc />
        public const string DefaultFileName = "CASaaS GRPC port";

        public const string DefaultCacheName = "DEFAULT";

        public const uint DefaultGracefulShutdownSeconds = 15;

        public LocalCasServiceSettings()
        {
        }

        public LocalCasServiceSettings(
            long defaultSingleInstanceTimeoutSec,
            uint gracefulShutdownSeconds = DefaultGracefulShutdownSeconds,
            string scenarioName = null,
            uint grpcPort = 0,
            string grpcPortFileName = null,
            int? bufferSizeForGrpcCopies = null
            )
        {
            DefaultSingleInstanceTimeoutSec = defaultSingleInstanceTimeoutSec;
            GracefulShutdownSeconds = gracefulShutdownSeconds;
            ScenarioName = scenarioName;
            GrpcPort = grpcPort;
            GrpcPortFileName = grpcPortFileName;
            BufferSizeForGrpcCopies = bufferSizeForGrpcCopies;
        }

        /// <summary>
        /// Default time to wait for an instance of the CAS to start up.
        /// </summary>
        [DataMember]
        public long DefaultSingleInstanceTimeoutSec { get; private set; }

        /// <summary>
        /// Server-side time allowed on shutdown for clients to gracefully close connections.
        /// </summary>
        [DataMember]
        public uint GracefulShutdownSeconds { get; set; } = DefaultGracefulShutdownSeconds;

        /// <summary>
        /// The GRPC port to use.
        /// </summary>
        [DataMember]
        public uint GrpcPort { get; set; }

        /// <summary>
        /// Name of the memory mapped file where the GRPC port number is saved.
        /// </summary>
        [DataMember]
        public string GrpcPortFileName { get; set; } = DefaultFileName;

        /// <summary>
        /// Name of the custom scenario that the CAS connects to.
        /// allows multiple CAS services to coexist in a machine
        /// since this factors into the cache root and the event that
        /// identifies a particular CAS instance.
        /// </summary>
        [DataMember]
        public string ScenarioName { get; set; }

        /// <summary>
        /// Period of inactivity after which sessions are shutdown and forgotten.
        /// </summary>
        [DataMember]
        public double? UnusedSessionTimeoutMinutes { get; set; } = null;

        /// <summary>
        /// Period of inactivity after which sessions with a heartbeat are shutdown and forgotten.
        /// </summary>
        [DataMember]
        public double? UnusedSessionHeartbeatTimeoutMinutes { get; set; } = null;

        /// <summary>
        /// Gets the buffer size used during streaming for GRPC copies.
        /// </summary>
        [DataMember]
        public int? BufferSizeForGrpcCopies { get; set; } = null;

        /// <summary>
        /// Gets or sets the max number of proactive pushes requests handled at the same time by a server.
        /// </summary>
        [DataMember]
        public int? MaxProactivePushRequestHandlers { get; set; }

        /// <summary>
        /// Gets or sets the max number of copy operations that can happen at the same time from this machine.
        /// </summary>
        [DataMember]
        public int? MaxCopyFromHandlers { get; set; }

        /// <summary>
        /// Whether to ensure long-running actions keep their sessions open.
        /// </summary>
        /// <remarks>
        /// If the heartbeat for a session is equals to the session's TTL, and a single operation runs longer then the TTL, then
        /// the backend will close the session due to the expiry.
        /// If this flag is set, the session won't be closed in this case.
        /// </remarks>
        [DataMember]
        public bool? DoNotShutdownSessionsInUse { get; set; }

        /// <summary>
        /// Whether to protect hibernated session data.
        /// </summary>
        [DataMember]
        public bool? ProtectHibernatedSessionData { get; set; }

        /// <nodoc />
        [DataMember]
        public GrpcCoreServerOptions GrpcCoreServerOptions { get; set; }

        /// <nodoc />
        [DataMember]
        public GrpcEnvironmentOptions GrpcEnvironmentOptions { get; set; }

        /// <summary>
        /// Returns true if gRPC.NET is used instead of gRPC.Core on the server side.
        /// </summary>
        [DataMember]
        public bool? UseGrpcDotNet { get; set; }

        /// <nodoc />
        [DataMember]
        public GrpcDotNetServerOptions GrpcDotNetServerOptions { get; set; }
    }
}
