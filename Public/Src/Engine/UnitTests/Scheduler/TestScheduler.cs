// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics.ContractsLight;
using System.Threading.Tasks;
using BuildXL.Engine.Cache;
using BuildXL.Ipc.Interfaces;
using BuildXL.Native.IO;
using BuildXL.Pips;
using BuildXL.Pips.Graph;
using BuildXL.Pips.Operations;
using BuildXL.Processes;
using BuildXL.Processes.VmCommandProxy;
using BuildXL.ProcessPipExecutor;
using BuildXL.Scheduler;
using BuildXL.Scheduler.Fingerprints;
using BuildXL.Storage;
using BuildXL.Utilities;
using BuildXL.Utilities.Core;
using BuildXL.Utilities.Configuration;
using BuildXL.Utilities.Instrumentation.Common;
using Test.BuildXL.TestUtilities.Xunit;

namespace Test.BuildXL.Scheduler
{
    public sealed class TestScheduler : global::BuildXL.Scheduler.Scheduler
    {
        private readonly Dictionary<PipId, PipResultStatus> m_overridePipResults = new Dictionary<PipId, PipResultStatus>();
        private readonly LoggingContext m_loggingContext;

        public ConcurrentDictionary<PipId, PipResultStatus> PipResults => RunData.PipResults;

        public ConcurrentDictionary<PipId, ObservedPathSet?> PathSets => RunData.PathSets;

        public ScheduleRunData RunData { get; } = new ScheduleRunData();

        public bool SandboxingWithKextEnabled => OperatingSystemHelper.IsUnixOS;

        protected override bool InitSandboxConnectionKext(LoggingContext loggingContext, ISandboxConnection SandboxConnectionKext = null)
        {
            if (SandboxingWithKextEnabled)
            {
                SandboxConnection = SandboxConnectionKext ?? XunitBuildXLTest.GetSandboxConnection();
            }

            return false;
        }

        private readonly TestPipQueue m_testPipQueue;

        public TestScheduler(
            PipGraph graph,
            TestPipQueue pipQueue,
            PipExecutionContext context,
            FileContentTable fileContentTable,
            EngineCache cache,
            IConfiguration configuration,
            FileAccessAllowlist fileAccessAllowlist,
            DirectoryMembershipFingerprinterRuleSet directoryMembershipFingerprinterRules = null,
            ITempCleaner tempCleaner = null,
            HistoricPerfDataTable runningTimeTable = null,
            JournalState journalState = null,
            PerformanceCollector performanceCollector = null,
            string fingerprintSalt = null,
            PreserveOutputsInfo? previousInputsSalt = null,
            IEnumerable<Pip> successfulPips = null,
            IEnumerable<Pip> failedPips = null,
            LoggingContext loggingContext = null,
            IIpcProvider ipcProvider = null,
            DirectoryTranslator directoryTranslator = null,
            VmInitializer vmInitializer = null,
            SchedulerTestHooks testHooks = null,
            FileTimestampTracker fileTimestampTracker = null) : base(graph, pipQueue, context, fileContentTable, cache,
                configuration, fileAccessAllowlist, loggingContext, null, directoryMembershipFingerprinterRules,
                tempCleaner, AsyncLazy<HistoricPerfDataTable>.FromResult(runningTimeTable), performanceCollector, fingerprintSalt, previousInputsSalt,
                ipcProvider: ipcProvider, 
                directoryTranslator: directoryTranslator, 
                journalState: journalState, 
                vmInitializer: vmInitializer,
                testHooks: testHooks,
                fileTimestampTracker: fileTimestampTracker)
        {
            m_testPipQueue = pipQueue;

            if (successfulPips != null)
            {
                foreach (var pip in successfulPips)
                {
                    Contract.Assume(pip.PipId.IsValid, "Override results must be added after the pip has been added to the scheduler");
                    m_overridePipResults.Add(pip.PipId, PipResultStatus.Succeeded);
                }
            }

            if (failedPips != null)
            {
                foreach (var pip in failedPips)
                {
                    Contract.Assume(pip.PipId.IsValid, "Override results must be added after the pip has been added to the scheduler");
                    m_overridePipResults.Add(pip.PipId, PipResultStatus.Failed);
                }
            }

            m_loggingContext = loggingContext;
        }

        public override async Task OnPipCompleted(RunnablePip runnablePip)
        {
            var pipId = runnablePip.Pip.PipId;
            PipResultStatus overrideStatus;
            if (m_overridePipResults.TryGetValue(pipId, out overrideStatus))
            {
                if (overrideStatus.IndicatesFailure())
                {
                    m_loggingContext.SpecifyErrorWasLogged(0);
                }

                runnablePip.SetPipResult(
                    overrideStatus.IndicatesExecution()
                        ? PipResult.CreateWithPointPerformanceInfo(overrideStatus)
                        : PipResult.CreateForNonExecution(overrideStatus));

                if (overrideStatus.IndicatesFailure())
                {
                    m_loggingContext.SpecifyErrorWasLogged(0);
                }
            }


            // Set the 'actual' result. NOTE: override also overrides actual result.
            // We set this before calling the wrapped PipCompleted handler since we may
            // be completing the last pip (don't want to race with a test checking pip
            // result after schedule completion and us setting it.
            PipResults[pipId] = runnablePip.Result.Value.Status;

            if (runnablePip.Result.HasValue && runnablePip.PipType == PipType.Process)
            {
                PathSets[pipId] = runnablePip.ExecutionResult?.PathSet;

                RunData.CacheLookupResults[pipId] = ((ProcessRunnablePip)runnablePip).CacheResult;
                RunData.ExecutionCachingInfos[pipId] = runnablePip.ExecutionResult?.TwoPhaseCachingInfo;
            }

            await base.OnPipCompleted(runnablePip);

            m_testPipQueue.OnPipCompleted(runnablePip.PipId);
        }

        public void AssertPipResults(
            Pip[] expectedSuccessfulPips = null,
            Pip[] expectedFailedPips = null,
            Pip[] expectedSkippedPips = null,
            Pip[] expectedCanceledPips = null,
            Pip[] expectedUnscheduledPips = null)
        {
            Dictionary<PipId, PipResultStatus?> expectedPipResults = new Dictionary<PipId, PipResultStatus?>();

            expectedSuccessfulPips = expectedSuccessfulPips ?? new Pip[0];
            expectedFailedPips = expectedFailedPips ?? new Pip[0];
            expectedSkippedPips = expectedSkippedPips ?? new Pip[0];
            expectedCanceledPips = expectedCanceledPips ?? new Pip[0];
            expectedUnscheduledPips = expectedUnscheduledPips ?? new Pip[0];

            foreach (var pip in expectedSuccessfulPips)
            {
                Contract.Assume(pip.PipId.IsValid, "Expected results must be added after the pip has been added to the scheduler");
                expectedPipResults.Add(pip.PipId, PipResultStatus.Succeeded);
            }

            foreach (var pip in expectedFailedPips)
            {
                Contract.Assume(pip.PipId.IsValid, "Expected results must be added after the pip has been added to the scheduler");
                expectedPipResults.Add(pip.PipId, PipResultStatus.Failed);
            }

            foreach (var pip in expectedSkippedPips)
            {
                Contract.Assume(pip.PipId.IsValid, "Expected results must be added after the pip has been added to the scheduler");
                expectedPipResults.Add(pip.PipId, PipResultStatus.Skipped);
            }

            foreach (var pip in expectedCanceledPips)
            {
                Contract.Assume(pip.PipId.IsValid, "Expected results must be added after the pip has been added to the scheduler");
                expectedPipResults.Add(pip.PipId, PipResultStatus.Canceled);
            }

            foreach (var pip in expectedUnscheduledPips)
            {
                Contract.Assume(pip.PipId.IsValid, "Expected results must be added after the pip has been added to the scheduler");
                expectedPipResults.Add(pip.PipId, null);
            }

            foreach (var expectedPipResult in expectedPipResults)
            {
                PipResultStatus actualPipResult;

                XAssert.AreEqual(expectedPipResult.Value.HasValue, PipResults.TryGetValue(expectedPipResult.Key, out actualPipResult));

                if (expectedPipResult.Value.HasValue)
                {
                    // Treat DeployedFromCache as Succeeded if that's what we wanted anyway; otherwise it is very hard to guess
                    // if a WriteFile / CopyFile pip will be satisfied from content-cache (many identical files in some tests).
                    if (actualPipResult == PipResultStatus.DeployedFromCache && expectedPipResult.Value.Value == PipResultStatus.Succeeded)
                    {
                        continue;
                    }

                    XAssert.AreEqual(expectedPipResult.Value.Value, actualPipResult);
                }
            }
        }
    }
}
