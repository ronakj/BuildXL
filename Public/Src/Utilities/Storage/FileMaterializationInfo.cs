// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System;
using BuildXL.Cache.ContentStore.Hashing;
using BuildXL.Native.IO;
using BuildXL.Utilities;
using BuildXL.Utilities.Core;
using static BuildXL.Utilities.Core.FormattableStringEx;

namespace BuildXL.Storage
{
    /// <summary>
    /// A <see cref="FileMaterializationInfo"/> is the combination of a file's known content hash and file name.
    /// </summary>
    public readonly struct FileMaterializationInfo : IEquatable<FileMaterializationInfo>
    {
        /// <summary>
        /// The file name of the file
        /// </summary>
        public readonly PathAtom FileName;

        /// <summary>
        /// Underlying <see cref="FileContentInfo"/> (hash and length of the corresponding file).
        /// </summary>
        public readonly FileContentInfo FileContentInfo;

        /// <summary>
        /// Checks whether reparse points is actionable, i.e., a mount point or a symlink.
        /// </summary>
        public bool IsReparsePointActionable => ReparsePointInfo.ReparsePointType.IsActionable();

        /// <summary>
        /// Checks whether the file's content should be cached. Returns true if the file is neither 
        /// a symlink / mount point <see cref="IsReparsePointActionable"/> nor a special-case hash.
        /// </summary>
        public bool IsCacheable => !IsReparsePointActionable && !Hash.IsSpecialValue();

        /// <summary>
        /// Underlying <see cref="ReparsePointInfo"/> (type and target (if available) of the reparse point).
        /// </summary>
        public readonly ReparsePointInfo ReparsePointInfo;

        /// <summary>
        /// Whether the file represents an allowed source rewrite
        /// </summary>
        public readonly bool IsUndeclaredFileRewrite;

        /// <summary>
        /// Whether the file has execution permission for the owner
        /// </summary>
        /// <remarks>
        /// Only valid in linux/mac OSs
        /// </remarks>
        public readonly bool IsExecutable;

        /// <summary>
        /// Creates a <see cref="FileMaterializationInfo"/> with an associated change tracking subscription.
        /// </summary>
        public FileMaterializationInfo(FileContentInfo fileContentInfo, PathAtom fileName, ReparsePointInfo? reparsePointInfo = null, bool isAllowedSourceRewrite = false, bool isExecutable = false)
        {
            FileName = fileName;
            FileContentInfo = fileContentInfo;
            ReparsePointInfo = reparsePointInfo ?? ReparsePointInfo.CreateNoneReparsePoint();
            IsUndeclaredFileRewrite = isAllowedSourceRewrite;
            IsExecutable = isExecutable;

            // NOTE: Update ExecutionResultSerializer WriteOutputContent/ReadOutputContent when adding new fields (i.e., BuildXL.Engine.Cache protobuf structure) 
            // NOTE: Update FileArtifactKeyedHash when adding new fields (i.e., BuildXL.Engine protobuf structure) 
        }

        /// <summary>
        /// Creates a <see cref="FileMaterializationInfo"/> with a hash but no file name and length.
        /// This is intended for abstract hashes that don't correspond to real files on disk.
        /// </summary>
        public static FileMaterializationInfo CreateWithUnknownLength(ContentHash hash)
        {
            return new FileMaterializationInfo(FileContentInfo.CreateWithUnknownLength(hash), PathAtom.Invalid);
        }

        /// <summary>
        /// Creates a <see cref="FileMaterializationInfo"/> with a hash but no file name.
        /// This is intended for abstract hashes that don't correspond to real files on disk.
        /// </summary>
        public static FileMaterializationInfo CreateWithUnknownName(in FileContentInfo contentInfo)
        {
            return new FileMaterializationInfo(contentInfo, PathAtom.Invalid);
        }

        /// <summary>
        /// Content hash of the file as of when tracking was started.
        /// </summary>
        public ContentHash Hash => FileContentInfo.Hash;

        /// <summary>
        /// Length of the file in bytes.
        /// </summary>
        /// <remarks>
        /// Do not use this value for serialization (use FileContentInfo.RawLength)
        /// </remarks>
        public long Length => FileContentInfo.Length;

        /// <inheritdoc />
        public override string ToString()
        {
            return I($"[Content {FileContentInfo} with file name '{FileName}']");
        }

        /// <inheritdoc />
        public bool Equals(FileMaterializationInfo other)
        {
            return other.FileName == FileName &&
                   other.FileContentInfo == FileContentInfo &&
                   other.ReparsePointInfo == ReparsePointInfo &&
                   other.IsUndeclaredFileRewrite == IsUndeclaredFileRewrite &&
                   other.IsExecutable == IsExecutable;
        }

        /// <inheritdoc />
        public override bool Equals(object obj)
        {
            return StructUtilities.Equals(this, obj);
        }

        /// <inheritdoc />
        public override int GetHashCode()
        {
            return HashCodeHelper.Combine(FileContentInfo.GetHashCode(), FileName.GetHashCode(), ReparsePointInfo.GetHashCode(), IsUndeclaredFileRewrite.GetHashCode(), IsExecutable.GetHashCode());
        }

        /// <nodoc />
        public static bool operator ==(FileMaterializationInfo left, FileMaterializationInfo right)
        {
            return left.Equals(right);
        }

        /// <nodoc />
        public static bool operator !=(FileMaterializationInfo left, FileMaterializationInfo right)
        {
            return !left.Equals(right);
        }
    }
}
