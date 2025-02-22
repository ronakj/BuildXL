// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System;
using System.Collections.Generic;
using System.Diagnostics.ContractsLight;
using System.Threading.Tasks;
using BuildXL.Utilities.Core;
using BuildXL.Utilities.Configuration;

namespace BuildXL.Cache.Interfaces.Test
{
    /// <summary>
    /// Factory for the CallbackCacheWrapper cache
    /// </summary>
    public class CallbackCacheFactory : ICacheFactory
    {
        private sealed class Config
        {
            /// <summary>
            /// Cache we're going to wrap
            /// </summary>
            public ICacheConfigData EncapsulatedCache { get; set; }
        }

        /// <inheritdoc/>
        public async Task<Possible<ICache, Failure>> InitializeCacheAsync(ICacheConfigData cacheData, Guid activityId, ICacheConfiguration cacheConfiguration = null)
        {
            Contract.Requires(cacheData != null);

            var possibleCacheConfig = cacheData.Create<Config>();
            if (!possibleCacheConfig.Succeeded)
            {
                return possibleCacheConfig.Failure;
            }

            Config cacheConfig = possibleCacheConfig.Result;

            var cache = await CacheFactory.InitializeCacheAsync(cacheConfig.EncapsulatedCache, activityId, cacheConfiguration);

            if (!cache.Succeeded)
            {
                return cache.Failure;
            }

            return new CallbackCacheWrapper(cache.Result);
        }
        
        /// <inheritdoc />
        public IEnumerable<Failure> ValidateConfiguration(ICacheConfigData cacheData)
            => CacheConfigDataValidator.ValidateConfiguration<Config>(cacheData, cacheConfig => CacheFactory.ValidateConfig(cacheConfig.EncapsulatedCache));
    }
}
