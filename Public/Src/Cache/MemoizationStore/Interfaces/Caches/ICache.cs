// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using BuildXL.Cache.ContentStore.Interfaces.Results;
using BuildXL.Cache.ContentStore.Interfaces.Stores;
using BuildXL.Cache.ContentStore.Interfaces.Tracing;
using BuildXL.Cache.MemoizationStore.Interfaces.Sessions;

namespace BuildXL.Cache.MemoizationStore.Interfaces.Caches
{
    /// <summary>
    ///     Standard interface for caches.
    /// </summary>
    public interface ICache : IStartupShutdown
    {
        /// <summary>
        ///     Gets the unique GUID for the given cache.
        /// </summary>
        /// <remarks>
        ///     It will be used also for storing who provided
        ///     the ViaCache determinism for memoization data.
        /// </remarks>
        Guid Id { get; }

        /// <summary>
        ///     Create a new session that can change the cache.
        /// </summary>
        CreateSessionResult<ICacheSession> CreateSession(Context context, string name, ImplicitPin implicitPin);

        /// <summary>
        ///     Gets a current stats snapshot.
        /// </summary>
        Task<GetStatsResult> GetStatsAsync(Context context);

        /// <summary>
        ///     Asynchronously enumerates the known strong fingerprints.
        /// </summary>
        IAsyncEnumerable<StructResult<StrongFingerprint>> EnumerateStrongFingerprints(Context context);
    }

    /// <nodoc />
    public interface IPublishingCache : ICache
    {
        /// <summary>
        ///     Create a writeable session that also publishes content hash lists to the remote.
        /// </summary>
        CreateSessionResult<ICacheSession> CreatePublishingSession(Context context, string name, ImplicitPin implicitPin, PublishingCacheConfiguration publishingConfig, string pat);
    }
}
