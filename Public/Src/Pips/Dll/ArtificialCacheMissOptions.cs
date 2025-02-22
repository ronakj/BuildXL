// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System;
using System.Collections.Generic;
using System.Diagnostics.ContractsLight;
using System.Globalization;
using BuildXL.Utilities.Core;
using BuildXL.Utilities.Collections;

namespace BuildXL.Pips
{
    /// <summary>
    /// Parameters for identifying pips that should be forced to have a cache-miss.
    /// </summary>
    /// <remarks>
    /// These parameters allow a configurable and optionally deterministic extra miss rate.
    /// This extra miss rate is useful for flushing out non-deterministic tool failures and
    /// bad cache entries (which may otherwise live for a long time).
    /// The randomizing parameters are string-representable as so: 
    /// 0.1 indicates "Miss rate 0.1; random seed"
    /// ~0.1 indicates "Miss rate 0.1; inverted; random seed"
    /// ~0.1#123 indicates "Miss rate 0.1 (10%); seed 123; inverted"
    /// There are the following properties regarding determinism:
    /// The same parameters for the same pip graph yields deterministic misses (i.e., same subset of pips with artificial
    /// misses).
    /// The inverted and non-inverted variants of the same miss-rate and seed are set-complements (in terms of the set of
    /// pips).
    /// For some fixed seed and graph, two miss rates A >= B give pip subsets P_A and P_B (the cache-missing pips) such that
    /// P_A is a superset of P_B.
    /// </remarks>
    public sealed class ArtificialCacheMissOptions
    {
        private readonly int m_seed;
        private readonly bool m_invert;
        private readonly double m_missRate;
        private readonly HashSet<long> m_forcedMisses = new();

        /// <summary>
        /// Creates miss-rate options with a random seed and no forced hashes.
        /// </summary>
        public ArtificialCacheMissOptions(double missRate, bool invert)
            : this(missRate, invert, Environment.TickCount, new())
        {
            Contract.Requires(missRate >= 0.0 && missRate <= 1.0);
        }

        /// <summary>
        /// Creates miss-rate options with a random seed and no forced hashes, including a seed.
        /// </summary>
        public ArtificialCacheMissOptions(double missRate, bool invert, int seed)
            : this(missRate, invert, seed, new())
        {
            Contract.Requires(missRate >= 0.0 && missRate <= 1.0);
        }


        /// <summary>
        /// Creates fully specified miss-rate options, including seed.
        /// </summary>
        /// <remarks>
        /// Given these exact parameters and the same pip graph, the same pips will have artificial misses.
        /// </remarks>
        public ArtificialCacheMissOptions(double missRate, bool invert, int seed, HashSet<long> forcedMisses)
        {
            Contract.Requires(missRate >= 0.0 && missRate <= 1.0);
            m_seed = seed;
            m_invert = invert;
            m_missRate = missRate;
            m_forcedMisses.AddRange(forcedMisses);
        }

        /// <summary>
        /// Seed determining the pip subset for the configured miss-rate (possibly random from creation).
        /// </summary>
        public int Seed => m_seed;

        /// <summary>
        /// Specified rate in the range [0.0, 1.0].
        /// </summary>
        public double Rate => m_missRate;

        /// <summary>
        /// Miss rate in the range [0.0, 1.0]; derived from the specified rate and whether or not the rate has been inverted.
        /// </summary>
        public double EffectiveMissRate => m_invert ? Math.Max(Math.Min(1.0 - m_missRate, 1.0), 0.0) : m_missRate;

        /// <summary>
        /// Indicates if a pip with the given semi-stable hash should have an artificial miss injected.
        /// </summary>
        public bool ShouldHaveArtificialMiss(long semiStableHash)
        {
            if (m_forcedMisses.Contains(semiStableHash))
            {
                return true;
            }

            var hash = unchecked((uint)HashCodeHelper.Combine(semiStableHash, m_seed));
            double percentageOfIntRange = (double)hash / (double)uint.MaxValue;
            return m_invert ^ (percentageOfIntRange <= m_missRate);
        }

        /// <nodoc />
        public string ToString(IFormatProvider formatProvider)
        {
            return string.Format(
                formatProvider,
                "{0}{1}#{2}",
                m_invert ? "~" : string.Empty,
                m_missRate,
                m_seed);
        }

        /// <nodoc />
        public override string ToString()
        {
            return ToString(CultureInfo.InvariantCulture);
        }
    }
}
