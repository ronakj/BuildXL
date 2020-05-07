// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System;
using System.Threading;
using System.Threading.Tasks;
using BuildXL.Cache.ContentStore.FileSystem;
using BuildXL.Cache.ContentStore.Interfaces.FileSystem;
using BuildXL.Cache.ContentStore.Interfaces.Logging;
using BuildXL.Cache.ContentStore.Interfaces.Results;
using BuildXL.Cache.ContentStore.Interfaces.Sessions;
using BuildXL.Cache.ContentStore.Interfaces.Tracing;
using BuildXL.Cache.ContentStore.Logging;
using BuildXL.Cache.ContentStore.Tracing;
using BuildXL.Cache.ContentStore.Tracing.Internal;
using BuildXL.Native.IO;
using BuildXL.Utilities.Tracing;

namespace BuildXL.Cache.ContentStore.Vfs
{
    using FullPath = Interfaces.FileSystem.AbsolutePath;
    using VirtualPath = System.String;

    /// <summary>
    /// A store which virtualizes calls to an underlying content store (i.e. content will
    /// be lazily materialized using the projected file system filter driver)
    /// </summary>
    internal class VfsContentManager : IDisposable
    {
        // TODO: Track stats about file materialization (i.e. how much content was hydrated)
        // On BuildXL side, track how much requested total requested file content size would be.

        public CounterCollection<VfsCounters> Counters { get; } = new CounterCollection<VfsCounters>();

        /// <summary>
        /// Unique integral id for files under vfs cas root
        /// </summary>
        private int _nextVfsCasTargetFileUniqueId;

        public VfsTree Tree { get; }
        private readonly VfsCasConfiguration _configuration;
        private readonly ILogger _logger;

        private readonly Tracer _tracer = new Tracer(nameof(VfsContentManager));

        private readonly IContentSession _contentSession;
        private readonly DisposableDirectory _tempDirectory;
        private readonly PassThroughFileSystem _fileSystem;

        public VfsContentManager(ILogger logger, VfsCasConfiguration configuration, VfsTree tree, IContentSession contentSession)
        {
            _logger = logger;
            _configuration = configuration;
            Tree = tree;
            _contentSession = contentSession;
            _fileSystem = new PassThroughFileSystem();
            _tempDirectory = new DisposableDirectory(_fileSystem, configuration.DataRootPath / "temp");
        }

        /// <summary>
        /// Converts the VFS root relative path to a full path
        /// </summary>
        public FullPath ToFullPath(VirtualPath relativePath)
        {
            return _configuration.VfsRootPath / relativePath;
        }

        /// <summary>
        /// Places a hydrated file at the given VFS root relative path
        /// </summary>
        /// <param name="relativePath">the vfs root relative path</param>
        /// <param name="data">the content and placement data for the file</param>
        /// <param name="token">the cancellation token</param>
        /// <returns>a task which completes when the operation is complete or throws an exception if error is encountered during operation</returns>
        public Task PlaceHydratedFileAsync(VirtualPath relativePath, VfsFilePlacementData data, CancellationToken token)
        {
            var context = new OperationContext(new Context(_logger), token);
            return context.PerformOperationAsync(
                _tracer,
                async () =>
                {
                    var tempFilePath = _tempDirectory.CreateRandomFileName();
                    var result = await _contentSession.PlaceFileAsync(
                        context,
                        data.Hash,
                        tempFilePath,
                        data.AccessMode,
                        FileReplacementMode.ReplaceExisting,
                        data.RealizationMode,
                        token).ThrowIfFailure();

                    var fullPath = ToFullPath(relativePath);

                    _fileSystem.MoveFile(tempFilePath, fullPath, true);

                    if (result.FileSize >= 0)
                    {
                        Counters[VfsCounters.PlaceHydratedFileBytes].Add(result.FileSize);
                    }
                    else
                    {
                        Counters[VfsCounters.PlaceHydratedFileUnknownSizeCount].Increment();
                    }

                    return BoolResult.Success;
                },
                extraStartMessage: $"RelativePath={relativePath}, Hash={data.Hash}",
                counter: Counters[VfsCounters.PlaceHydratedFile]);
        }

        /// <summary>
        /// Converts the full path to a VFS root relative path
        /// </summary>
        internal VirtualPath ToVirtualPath(FullPath path)
        {
            foreach (var mount in _configuration.VirtualizationMounts)
            {
                if (path.Path.TryGetRelativePath(mount.Value.Path, out var mountRelativePath))
                {
                    RelativePath relativePath = _configuration.VfsMountRelativeRoot / mount.Key / mountRelativePath;
                    return relativePath.Path;
                }
            }

            if (path.Path.TryGetRelativePath(_configuration.VfsRootPath.Path, out var rootRelativePath))
            {
                return rootRelativePath;
            }

            return null;
        }

        internal Result<VirtualPath> TryCreateSymlink(OperationContext context, AbsolutePath sourcePath, VfsFilePlacementData data, bool replace)
        {
            return context.PerformOperation(
                _tracer,
                () =>
                {
                    _fileSystem.CreateDirectory(sourcePath.Parent);

                    if (replace)
                    {
                        FileUtilities.DeleteFile(sourcePath.Path);
                    }

                    var index = Interlocked.Increment(ref _nextVfsCasTargetFileUniqueId);
                    VirtualPath casRelativePath = VfsUtilities.CreateCasRelativePath(data, index);

                    var virtualPath = _configuration.VfsCasRelativeRoot / casRelativePath;

                    Tree.AddFileNode(virtualPath.Path, data);
                    // Ensure existence of the virtual directory in the VFS CAS root
                    //Tree.GetOrAddDirectoryNode((_configuration.VfsCasRelativeRoot / casRelativePath).Parent.Path);

                    var fullTargetPath = _configuration.VfsCasRootPath / casRelativePath;
                    var result = FileUtilities.TryCreateSymbolicLink(symLinkFileName: sourcePath.Path, targetFileName: fullTargetPath.Path, isTargetFile: true);
                    if (result.Succeeded)
                    {
                        return Result.Success(virtualPath.Path);
                    }
                    else
                    {
                        return Result.FromErrorMessage<VirtualPath>(result.Failure.DescribeIncludingInnerFailures());
                    }
                },
                extraStartMessage: $"SourcePath={sourcePath}, Hash={data.Hash}",
                messageFactory: r => $"SourcePath={sourcePath}, Hash={data.Hash}, TargetPath={r.GetValueOrDefault()}",
                counter: Counters[VfsCounters.TryCreateSymlink]);
        }

        /// <inheritdoc />
        public void Dispose()
        {
            _tempDirectory.Dispose();
        }
    }
}
