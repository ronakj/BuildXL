// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Diagnostics.ContractsLight;

namespace BuildXL.Utilities.Configuration.Mutable
{
    /// <nodoc />
    public sealed class NugetResolverSettings : ResolverSettings, INugetResolverSettings
    {
        /// <nodoc />
        public NugetResolverSettings()
        {
            Repositories = new Dictionary<string, string>();
            Packages = new List<INugetPackage>();
            DoNotEnforceDependencyVersions = false;
            Configuration = new NugetConfiguration();
            EsrpSignConfiguration = null;
        }

        /// <nodoc />
        public NugetResolverSettings(INugetResolverSettings template, PathRemapper pathRemapper)
            : base(template, pathRemapper)
        {
            Contract.Assume(template != null);
            Contract.Assume(pathRemapper != null);

            Configuration = new NugetConfiguration(template.Configuration);
            Repositories = new Dictionary<string, string>(template.Repositories.Count);
            foreach (var kv in template.Repositories)
            {
                Repositories.Add(kv.Key, kv.Value);
            }

            Packages = new List<INugetPackage>(template.Packages.Count);
            foreach (var package in template.Packages)
            {
                Packages.Add(new NugetPackage(package));
            }

            DoNotEnforceDependencyVersions = template.DoNotEnforceDependencyVersions;
            EsrpSignConfiguration = template.EsrpSignConfiguration == null 
                ? null : new EsrpSignConfiguration(template.EsrpSignConfiguration, pathRemapper);
        }

        /// <inheritdoc />
        public INugetConfiguration Configuration { get; set; }

        /// <nodoc />
        [SuppressMessage("Microsoft.Usage", "CA2227:CollectionPropertiesShouldBeReadOnly")]
        public Dictionary<string, string> Repositories { get; set; }

        /// <inheritdoc />
        IReadOnlyDictionary<string, string> INugetResolverSettings.Repositories => Repositories;

        /// <nodoc />
        [SuppressMessage("Microsoft.Usage", "CA2227:CollectionPropertiesShouldBeReadOnly")]
        public List<INugetPackage> Packages { get; set; }

        /// <inheritdoc/>
        public bool DoNotEnforceDependencyVersions { get; set; }

        /// <inheritdoc />
        IReadOnlyList<INugetPackage> INugetResolverSettings.Packages => Packages;

        /// <inheritdoc />
        public IEsrpSignConfiguration EsrpSignConfiguration { get; set; }
    }
}
