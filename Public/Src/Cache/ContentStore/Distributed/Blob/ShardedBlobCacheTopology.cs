﻿// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System;
using System.Collections.Concurrent;
using System.Diagnostics.ContractsLight;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Azure;
using Azure.Storage.Blobs;
using BuildXL.Cache.ContentStore.Interfaces.Results;
using BuildXL.Cache.ContentStore.Synchronization;
using BuildXL.Cache.ContentStore.Tracing;
using BuildXL.Cache.ContentStore.Tracing.Internal;

#nullable enable

namespace BuildXL.Cache.ContentStore.Distributed.Blob;

public class ShardedBlobCacheTopology : IBlobCacheTopology
{
    protected Tracer Tracer { get; } = new Tracer(nameof(ShardedBlobCacheTopology));

    public record Configuration(
        ShardingScheme ShardingScheme,
        IBlobCacheSecretsProvider SecretsProvider,
        string Universe,
        string Namespace,
        TimeSpan? ClientCreationTimeout = null);

    private readonly Configuration _configuration;

    /// <summary>
    /// Holds pre-allocated container names to avoid allocating strings every time we want to get the container for a
    /// given key.
    /// </summary>
    private readonly BlobCacheContainerName[] _containers;
    private readonly IShardingScheme<int, BlobCacheStorageAccountName> _scheme;

    private readonly record struct Location(BlobCacheStorageAccountName Account, BlobCacheContainerName Container);

    /// <summary>
    /// Used to implement a double-checked locking pattern at the per-container level. Essentially, we don't want to
    /// waste resources by creating clients for the same container at the same time.
    /// </summary>
    private readonly LockSet<Location> _locks = new();

    /// <summary>
    /// We cache the clients because:
    /// 1. Obtaining clients requires obtaining storage credentials, which may or may not involve RPCs.
    /// 2. Once the storage credential has been obtained, we should be fine re-using it.
    /// 3. It is possible (although we don't know) that the blob objects have internal state about connections that is
    ///    better to share.
    /// </summary>
    private readonly ConcurrentDictionary<Location, BlobContainerClient> _clients = new();

    public ShardedBlobCacheTopology(Configuration configuration)
    {
        _configuration = configuration;

        _scheme = _configuration.ShardingScheme.Create();

        _containers = Enum.GetValues(typeof(BlobCacheContainerPurpose)).Cast<BlobCacheContainerPurpose>().Select(
            purpose => new BlobCacheContainerName(
                BlobCacheVersion.V0,
                purpose,
                _configuration.Universe,
                _configuration.Namespace)).ToArray();
    }

    public async Task<BlobContainerClient> GetContainerClientAsync(OperationContext context, BlobCacheShardingKey key)
    {
        var account = _scheme.Locate(key.Key);

        // _containers is created with this same enum, so this index access is safe.
        var container = _containers[(int)key.Purpose];

        var location = new Location(account, container);

        // NOTE: We don't use AddOrGet because CreateClientAsync could fail, in which case we'd have a task that would
        // fail everyone using this.
        if (_clients.TryGetValue(location, out var client))
        {
            return client;
        }

        using var guard = await _locks.AcquireAsync(location, context.Token);
        if (_clients.TryGetValue(location, out client))
        {
            return client;
        }

        client = await CreateClientAsync(context, account, container).ThrowIfFailureAsync();

        var added = _clients.TryAdd(location, client);
        Contract.Assert(added, "Impossible condition happened: lost TryAdd race under a lock");

        return client;
    }

    private Task<Result<BlobContainerClient>> CreateClientAsync(OperationContext context, BlobCacheStorageAccountName account, BlobCacheContainerName container)
    {
        var msg = $"Account=[{account}] Container=[{container}]";
        return context.PerformOperationWithTimeoutAsync(
            Tracer,
            async context =>
            {
                var credentials = await _configuration.SecretsProvider.RetrieveBlobCredentialsAsync(context, account, container);
                var containerClient = credentials.CreateContainerClient(container.ContainerName);

                try
                {
                    await containerClient.CreateIfNotExistsAsync(
                        Azure.Storage.Blobs.Models.PublicAccessType.None,
                        null,
                        null,
                        cancellationToken: context.Token);
                }
                catch (RequestFailedException exception)
                {
                    throw new InvalidOperationException(message: $"Container `{container}` does not exist in account `{account}` and could not be created", innerException: exception);
                }

                return Result.Success(containerClient);
            },
            extraStartMessage: msg,
            extraEndMessage: _ => msg,
            timeout: _configuration.ClientCreationTimeout ?? Timeout.InfiniteTimeSpan);
    }
}
