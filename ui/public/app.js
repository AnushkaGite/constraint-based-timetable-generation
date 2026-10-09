/**
 * ui/public/app.js — Interactive Timetable Dashboard & Benchmark Analytics
 */

// ── Application State ─────────────────────────────────────────────────
const state = {
  currentTab: 'tabTimetable',
  currentRun: null,
  activeFilterMode: 'all', // 'all', 'batch', 'faculty', 'room'
  activeFilterValue: '',
  benchData: [],
  benchFilterSize: 'ALL',
  benchSortCol: 'dataset',
  benchSortAsc: true,
  charts: {},
  activeJobId: null,
  jobPollTimer: null,
  jobStartTime: 0,
  cpuCount: 4,
  theme: 'dark'
};

// ── Helpers ───────────────────────────────────────────────────────────
function formatMs(ms) {
  if (ms == null || isNaN(ms)) return '—';
  if (ms < 1000) return `${ms.toFixed(1)} ms`;
  return `${(ms / 1000).toFixed(2)} s`;
}

function formatNum(n) {
  if (n == null || isNaN(n)) return '—';
  return Number(n).toLocaleString();
}

function showToast(msg, type = 'info') {
  const container = document.getElementById('toastContainer');
  const toast = document.createElement('div');
  toast.className = `toast toast-${type}`;
  toast.innerHTML = `<span>${type === 'success' ? '✅' : type === 'error' ? '❌' : 'ℹ️'}</span> <span>${msg}</span>`;
  container.appendChild(toast);
  setTimeout(() => {
    toast.style.opacity = '0';
    setTimeout(() => toast.remove(), 200);
  }, 4000);
}

// ── Initialization ────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
  initTheme();
  initTabs();
  initControls();
  initSideDrawer();
  checkHealth();
  loadSavedRunsList();
  loadBenchmarks();
});

// ── Theme Management ──────────────────────────────────────────────────
function initTheme() {
  const saved = localStorage.getItem('timetable_theme');
  const prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
  state.theme = saved || (prefersDark ? 'dark' : 'light');
  document.body.dataset.theme = state.theme;
  updateThemeIcon();

  document.getElementById('btnThemeToggle').addEventListener('click', () => {
    state.theme = state.theme === 'dark' ? 'light' : 'dark';
    document.body.dataset.theme = state.theme;
    localStorage.setItem('timetable_theme', state.theme);
    updateThemeIcon();
    // Update chart colors on theme toggle
    if (state.currentTab === 'tabBenchmarks') renderBenchmarkCharts();
    if (state.currentTab === 'tabScaling') renderScalingCharts();
  });
}

function updateThemeIcon() {
  const icon = document.getElementById('themeIcon');
  icon.textContent = state.theme === 'dark' ? '☀️' : '🌙';
}

// ── Tabs Navigation ───────────────────────────────────────────────────
function initTabs() {
  const tabBtns = document.querySelectorAll('.tab-btn');
  tabBtns.forEach(btn => {
    btn.addEventListener('click', () => {
      const target = btn.dataset.tab;
      switchTab(target);
    });
  });
}

function switchTab(tabId) {
  state.currentTab = tabId;
  document.querySelectorAll('.tab-btn').forEach(b => {
    const active = b.dataset.tab === tabId;
    b.classList.toggle('active', active);
    b.setAttribute('aria-selected', active);
  });
  document.querySelectorAll('.tab-panel').forEach(p => {
    p.classList.toggle('active', p.id === tabId);
    p.style.display = p.id === tabId ? 'flex' : 'none';
  });

  if (tabId === 'tabBenchmarks') {
    loadBenchmarks();
  }
}

// ── Health Check ──────────────────────────────────────────────────────
async function checkHealth() {
  try {
    const res = await fetch('/api/health');
    if (!res.ok) throw new Error('Server returned ' + res.status);
    const data = await res.json();

    // Server chip
    const chipServer = document.getElementById('chipServer');
    chipServer.innerHTML = `<span class="status-dot dot-green"></span><span>Server (v${data.pythonVersion})</span>`;
    chipServer.title = `Running on ${data.platform}`;

    // Exe chip
    const chipExe = document.getElementById('chipExe');
    if (data.exeFound) {
      chipExe.innerHTML = `<span class="status-dot dot-green"></span><span>Solver EXE</span>`;
      chipExe.title = `Found at ${data.exePath}`;
    } else {
      chipExe.innerHTML = `<span class="status-dot dot-red"></span><span>Solver Missing</span>`;
      chipExe.title = `Not found! Build command: ${data.buildHelp}`;
    }

    // CSV chip
    const chipCsv = document.getElementById('chipCsv');
    if (data.csvFound) {
      chipCsv.innerHTML = `<span class="status-dot dot-green"></span><span>Dataset CSV</span>`;
      chipCsv.title = `Found at ${data.csvPath}`;
    } else {
      chipCsv.innerHTML = `<span class="status-dot dot-red"></span><span>CSV Missing</span>`;
      chipCsv.title = `timetable_dataset.csv not found`;
    }

    if (data.cpuCount) {
      state.cpuCount = data.cpuCount;
      const numThreads = document.getElementById('numThreads');
      numThreads.max = data.cpuCount;
      numThreads.value = Math.min(4, data.cpuCount);
      // Update Tab 3 thread 8 checkbox
      const chk8 = document.getElementById('lblThread8');
      if (chk8 && data.cpuCount < 8) chk8.style.display = 'none';
    }
  } catch (err) {
    const chipServer = document.getElementById('chipServer');
    chipServer.innerHTML = `<span class="status-dot dot-red"></span><span>Server Offline</span>`;
    showToast('Cannot connect to Python server', 'error');
  }
}

// ── Controls & Solver Execution ───────────────────────────────────────
function initControls() {
  const selAlgo = document.getElementById('selAlgo');
  const grpThreads = document.getElementById('grpThreads');
  const grpTimeLimit = document.getElementById('grpTimeLimit');
  const selTimeLimit = document.getElementById('selTimeLimit');
  const customTimeLimit = document.getElementById('customTimeLimit');

  function updateAlgoVisibility() {
    const algo = selAlgo.value;
    grpThreads.style.display = (algo === 'pbnb') ? 'flex' : 'none';
    grpTimeLimit.style.display = (algo === 'greedy') ? 'none' : 'flex';
  }
  selAlgo.addEventListener('change', updateAlgoVisibility);
  updateAlgoVisibility();

  selTimeLimit.addEventListener('change', () => {
    customTimeLimit.style.display = (selTimeLimit.value === 'custom') ? 'inline-block' : 'none';
  });

  // Run Solver Button
  document.getElementById('btnRunSolver').addEventListener('click', startSolverRun);
  document.getElementById('btnCancelJob').addEventListener('click', cancelActiveJob);

  // Saved runs select
  document.getElementById('selSavedRuns').addEventListener('change', (e) => {
    if (e.target.value) loadRunById(e.target.value);
  });

  // Filter mode buttons
  document.querySelectorAll('.btn-filter[data-mode]').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.btn-filter[data-mode]').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      state.activeFilterMode = btn.dataset.mode;
      updateFilterDropdown();
      renderTimetableGrid();
    });
  });

  document.getElementById('selFilterTarget').addEventListener('change', (e) => {
    state.activeFilterValue = e.target.value;
    renderTimetableGrid();
  });

  // Benchmarks filters
  document.querySelectorAll('#benchSizeFilters .btn-filter').forEach(btn => {
    btn.addEventListener('click', () => {
      if (btn.classList.contains('btn-disabled')) return;
      document.querySelectorAll('#benchSizeFilters .btn-filter').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      state.benchFilterSize = btn.dataset.size;
      renderBenchmarkCharts();
      renderBenchmarkTable();
    });
  });

  document.getElementById('btnDownloadCsv').addEventListener('click', downloadBenchmarkCsv);
  document.getElementById('btnRefreshBench').addEventListener('click', loadBenchmarks);

  // Scaling sweep controls
  document.getElementById('btnStartSweep').addEventListener('click', startScalingSweep);
  document.getElementById('btnCancelSweep').addEventListener('click', cancelActiveJob);
}

// ── Live Solver Dispatch & Polling ────────────────────────────────────
async function startSolverRun() {
  const size = document.getElementById('selSize').value;
  const algo = document.getElementById('selAlgo').value;
  const threads = parseInt(document.getElementById('numThreads').value, 10) || 4;

  let timeLimitMs = 30000;
  const limitChoice = document.getElementById('selTimeLimit').value;
  if (limitChoice === 'custom') {
    timeLimitMs = parseInt(document.getElementById('customTimeLimit').value, 10) || 30000;
  } else {
    timeLimitMs = parseInt(limitChoice, 10) || 30000;
  }

  const payload = { size, algo, threads, timeLimitMs };

  try {
    const res = await fetch('/api/run', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    if (res.status === 409) {
      showToast('Another solver run is currently active!', 'error');
      return;
    }
    if (!res.ok) {
      const err = await res.json();
      throw new Error(err.error || 'Failed to start run');
    }
    const data = await res.json();
    state.activeJobId = data.jobId;
    state.jobStartTime = Date.now();

    setSolverRunningUI(true, `Running ${algo.toUpperCase()} on ${size}...`);
    startJobPolling(data.jobId, (finalJob) => {
      setSolverRunningUI(false);
      if (finalJob.status === 'done' && finalJob.runId) {
        showToast('Timetable solver finished successfully!', 'success');
        loadSavedRunsList();
        loadRunById(finalJob.runId);
      } else if (finalJob.status === 'cancelled') {
        showToast('Run was cancelled', 'info');
      } else {
        showToast(`Solver error: ${finalJob.error || 'Execution failed'}`, 'error');
      }
    });

  } catch (err) {
    showToast(err.message, 'error');
  }
}

function setSolverRunningUI(running, message = '') {
  const btnRun = document.getElementById('btnRunSolver');
  const btnCancel = document.getElementById('btnCancelJob');
  const bar = document.getElementById('jobProgressBar');
  const text = document.getElementById('jobProgressText');
  const timer = document.getElementById('jobTimer');

  if (running) {
    btnRun.disabled = true;
    btnCancel.style.display = 'inline-flex';
    bar.style.display = 'flex';
    text.textContent = message;
    timer.textContent = '0.0s';
  } else {
    btnRun.disabled = false;
    btnCancel.style.display = 'none';
    bar.style.display = 'none';
    if (state.jobPollTimer) {
      clearInterval(state.jobPollTimer);
      state.jobPollTimer = null;
    }
  }
}

function startJobPolling(jobId, onComplete) {
  if (state.jobPollTimer) clearInterval(state.jobPollTimer);

  state.jobPollTimer = setInterval(async () => {
    try {
      const res = await fetch(`/api/jobs/${jobId}`);
      if (!res.ok) {
        clearInterval(state.jobPollTimer);
        onComplete({ status: 'error', error: 'Lost connection to job' });
        return;
      }
      const job = await res.json();

      // Update timer
      const elapsed = ((Date.now() - state.jobStartTime) / 1000).toFixed(1);
      const timer = document.getElementById('jobTimer');
      if (timer) timer.textContent = `${elapsed}s`;

      const sweepTimer = document.getElementById('sweepStepCounter');
      if (sweepTimer && job.totalSteps > 1) {
        sweepTimer.textContent = `Step ${job.currentStep || 1} / ${job.totalSteps}: ${job.stepDesc || ''}`;
      }

      if (job.status !== 'running') {
        clearInterval(state.jobPollTimer);
        state.jobPollTimer = null;
        onComplete(job);
      }
    } catch (e) {
      clearInterval(state.jobPollTimer);
      onComplete({ status: 'error', error: e.message });
    }
  }, 250);
}

async function cancelActiveJob() {
  if (!state.activeJobId) return;
  try {
    const res = await fetch(`/api/jobs/${state.activeJobId}/cancel`, { method: 'POST' });
    if (res.ok) {
      showToast('Cancellation requested...', 'info');
    }
  } catch (e) {
    showToast('Failed to cancel: ' + e.message, 'error');
  }
}

// ── Saved Runs Management ─────────────────────────────────────────────
async function loadSavedRunsList() {
  try {
    const res = await fetch('/api/runs');
    if (!res.ok) return;
    const runs = await res.json();
    const select = document.getElementById('selSavedRuns');
    select.innerHTML = '<option value="">-- Choose a saved run --</option>';

    runs.forEach(r => {
      const opt = document.createElement('option');
      opt.value = r.id;
      const statusIcon = r.feasible ? '🟢' : (r.scheduled > 0 ? '🟡' : '🔴');
      opt.textContent = `${statusIcon} [${r.dataset}] ${r.algorithm.toUpperCase()} (t=${r.threads}) - ${r.dateStr}`;
      select.appendChild(opt);
    });
  } catch (err) {
    console.warn('Could not load saved runs:', err);
  }
}

async function loadRunById(runId) {
  try {
    const res = await fetch(`/api/runs/${runId}`);
    if (!res.ok) throw new Error('Run not found');
    const runData = await res.json();
    state.currentRun = runData;
    renderRunResults(runData);
  } catch (err) {
    showToast(err.message, 'error');
  }
}

// ── Result Rendering ──────────────────────────────────────────────────
function renderRunResults(data) {
  const resultsSection = document.getElementById('resultsSection');
  const emptyState = document.getElementById('emptyStateTimetable');
  resultsSection.style.display = 'block';
  emptyState.style.display = 'none';

  const meta = data.meta || {};
  const metrics = data.metrics || {};
  const val = data.validation || {};

  // 1. Feasibility Badge Logic (computed from data, not just flag)
  const badge = document.getElementById('feasibilityBadge');
  const isComplete = (metrics.unscheduled === 0) && val.feasible;
  const isPartial = (metrics.scheduled > 0) && (metrics.unscheduled > 0);
  const isNoTimetable = (metrics.scheduled === 0);

  if (isComplete) {
    badge.className = 'badge badge-feasible';
    badge.textContent = '🟢 FEASIBLE — 0 Hard Violations';
  } else if (isPartial) {
    badge.className = 'badge badge-partial';
    badge.textContent = `🟡 PARTIAL — ${metrics.scheduled}/${metrics.sessions} Scheduled`;
  } else if (isNoTimetable) {
    badge.className = 'badge badge-infeasible';
    badge.textContent = '🔴 NO TIMETABLE — Budget Exhausted Before Solution';
  } else {
    badge.className = 'badge badge-infeasible';
    badge.textContent = '🔴 INFEASIBLE';
  }

  // Header Title & Meta
  document.getElementById('runMetaHeading').textContent =
    `${meta.dataset} — ${meta.algorithm.toUpperCase()}`;
  document.getElementById('runMetaSubtitle').textContent =
    `Threads: ${meta.threads} | Limit: ${meta.timeLimitMs ? meta.timeLimitMs + 'ms' : 'N/A'}`;

  // Greedy Fallback notice
  const fallbackNotice = document.getElementById('fallbackNotice');
  fallbackNotice.style.display = metrics.usedGreedyFallback ? 'block' : 'none';

  // Metrics Cards
  document.getElementById('valRuntime').textContent = formatMs(metrics.runtimeMs);
  document.getElementById('valScheduled').textContent = `${metrics.scheduled} / ${metrics.sessions}`;
  document.getElementById('valPenalty').textContent = metrics.penalty != null ? formatNum(metrics.penalty) : '—';
  document.getElementById('valNodes').textContent = formatNum(metrics.nodes);
  document.getElementById('valBacktracks').textContent = formatNum(metrics.backtracks);
  document.getElementById('valPruned').textContent = formatNum(metrics.pruned);

  // Hide metric cards that are null
  document.getElementById('cardNodes').style.display = metrics.nodes != null ? 'flex' : 'none';
  document.getElementById('cardBacktracks').style.display = metrics.backtracks != null ? 'flex' : 'none';
  document.getElementById('cardPruned').style.display = metrics.pruned != null ? 'flex' : 'none';

  // 2. Constraint Breakdown Table
  renderConstraintTable(val);

  // 3. Unscheduled Sessions Panel
  const unscheduledPanel = document.getElementById('unscheduledPanel');
  const unscheduledList = document.getElementById('unscheduledList');
  if (metrics.unscheduled > 0) {
    unscheduledPanel.style.display = 'block';
    unscheduledList.innerHTML = '';
    const unassigned = (data.sessions || []).filter(s => !s.assigned);
    unassigned.forEach(s => {
      const chip = document.createElement('div');
      chip.className = 'unscheduled-chip';
      chip.innerHTML = `<strong>${s.sessionId}</strong>: ${s.courseName} (${s.facultyId} • ${s.batchId})`;
      unscheduledList.appendChild(chip);
    });
  } else {
    unscheduledPanel.style.display = 'none';
  }

  // 4. Update Filter Dropdown & Render Grid
  updateFilterDropdown();
  renderTimetableGrid();
}

function renderConstraintTable(val) {
  const tbody = document.getElementById('constraintTableBody');
  const summaryTag = document.getElementById('constraintSummaryTag');

  const rules = [
    { name: 'Faculty Conflicts', key: 'facultyConflicts', desc: 'No faculty member double-booked across overlapping slots' },
    { name: 'Batch Conflicts', key: 'batchConflicts', desc: 'No student batch assigned to multiple courses simultaneously' },
    { name: 'Room Conflicts', key: 'roomConflicts', desc: 'No room assigned to multiple sessions simultaneously' },
    { name: 'Capacity Violations', key: 'capacityViolations', desc: 'Room capacity must be greater or equal to batch strength' },
    { name: 'Room Type Violations', key: 'roomTypeViolations', desc: 'LAB courses in LAB rooms, THEORY in THEORY rooms' },
    { name: 'Availability Violations', key: 'availabilityViolations', desc: 'Faculty and batch must be available in assigned slots' },
    { name: 'Lab Duration & Lunch', key: 'labViolations', desc: 'Labs must be 2 slots and must not span the lunch break (slot 4–5)' },
    { name: 'Missing Sessions', key: 'missingSessions', desc: 'Every required curriculum session must have a valid slot & room' },
  ];

  let totalViolations = 0;
  tbody.innerHTML = '';

  rules.forEach(r => {
    const count = val[r.key] || 0;
    totalViolations += count;
    const isPass = count === 0;

    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td><strong>${r.name}</strong></td>
      <td><span class="badge ${isPass ? 'badge-feasible' : 'badge-infeasible'}">${isPass ? 'PASS' : 'VIOLATION'}</span></td>
      <td class="${isPass ? 'violation-count-zero' : 'violation-count-bad'}">${count}</td>
      <td class="text-muted">${r.desc}</td>
    `;
    tbody.appendChild(tr);
  });

  if (totalViolations === 0 && val.feasible) {
    summaryTag.className = 'summary-status-tag badge-feasible';
    summaryTag.textContent = '100% Constraints Satisfied';
  } else {
    summaryTag.className = 'summary-status-tag badge-infeasible';
    summaryTag.textContent = `${totalViolations} Hard Violations Detected`;
  }
}

// ── Filter Controls for Grid ──────────────────────────────────────────
function updateFilterDropdown() {
  const group = document.getElementById('filterDropdownGroup');
  const label = document.getElementById('lblFilterTarget');
  const select = document.getElementById('selFilterTarget');
  const sessions = state.currentRun ? (state.currentRun.sessions || []) : [];

  if (state.activeFilterMode === 'all') {
    group.style.display = 'none';
    state.activeFilterValue = '';
    return;
  }

  group.style.display = 'flex';
  select.innerHTML = '';

  let items = new Set();
  if (state.activeFilterMode === 'batch') {
    label.textContent = 'Select Batch:';
    sessions.forEach(s => s.batchId && items.add(s.batchId));
  } else if (state.activeFilterMode === 'faculty') {
    label.textContent = 'Select Faculty:';
    sessions.forEach(s => s.facultyId && items.add(s.facultyId));
  } else if (state.activeFilterMode === 'room') {
    label.textContent = 'Select Room:';
    sessions.forEach(s => s.roomId && items.add(s.roomId));
  }

  const sorted = Array.from(items).sort();
  sorted.forEach(it => {
    const opt = document.createElement('option');
    opt.value = it;
    opt.textContent = it;
    select.appendChild(opt);
  });

  if (sorted.length > 0) {
    state.activeFilterValue = sorted[0];
    select.value = sorted[0];
  } else {
    state.activeFilterValue = '';
  }
}

// ── Timetable Grid Matrix ─────────────────────────────────────────────
function renderTimetableGrid() {
  const grid = document.getElementById('timetableGrid');
  if (!state.currentRun) return;

  const defaultSlotLabels = [
    '09:00 - 10:00',
    '10:00 - 11:00',
    '11:00 - 12:00',
    '12:00 - 13:00',
    '14:00 - 15:00',
    '15:00 - 16:00',
    '16:00 - 17:00'
  ];

  const constants = state.currentRun.constants || {
    days: ['MON', 'TUE', 'WED', 'THU', 'FRI'],
    slotsPerDay: 7,
    lunchAfterSlot: 4,
    slotLabels: defaultSlotLabels
  };
  const sessions = state.currentRun.sessions || [];

  // Filter assigned sessions
  const assigned = sessions.filter(s => s.assigned);
  let visibleSessions = assigned;

  if (state.activeFilterMode === 'batch' && state.activeFilterValue) {
    visibleSessions = assigned.filter(s => s.batchId === state.activeFilterValue);
  } else if (state.activeFilterMode === 'faculty' && state.activeFilterValue) {
    visibleSessions = assigned.filter(s => s.facultyId === state.activeFilterValue);
  } else if (state.activeFilterMode === 'room' && state.activeFilterValue) {
    visibleSessions = assigned.filter(s => s.roomId === state.activeFilterValue);
  }

  document.getElementById('totalVisibleSessions').textContent =
    `Showing ${visibleSessions.length} of ${assigned.length} assigned`;

  grid.innerHTML = '';

  // 1. Header Row
  const timeHeader = document.createElement('div');
  timeHeader.className = 'grid-cell grid-header-cell';
  timeHeader.textContent = 'TIME / DAY';
  grid.appendChild(timeHeader);

  constants.days.forEach(day => {
    const dayHeader = document.createElement('div');
    dayHeader.className = 'grid-cell grid-header-cell';
    dayHeader.textContent = day;
    grid.appendChild(dayHeader);
  });

  // Helper map: [day][slot] -> list of sessions
  const cellMap = {};
  constants.days.forEach(d => {
    cellMap[d] = {};
    for (let sl = 1; sl <= constants.slotsPerDay; ++sl) cellMap[d][sl] = [];
  });

  visibleSessions.forEach(s => {
    if (s.day && cellMap[s.day] && cellMap[s.day][s.startSlot]) {
      cellMap[s.day][s.startSlot].push(s);
    }
  });

  // 2. Rows: Slots 1 to 7
  for (let slot = 1; slot <= constants.slotsPerDay; ++slot) {

    // Lunch Break Divider before slot 5
    if (slot === constants.lunchAfterSlot + 1) {
      const lunchDiv = document.createElement('div');
      lunchDiv.className = 'grid-lunch-divider';
      lunchDiv.innerHTML = `🍽️ LUNCH BREAK (13:00 - 14:00) — Crossing Prohibited for Lab Sessions`;
      grid.appendChild(lunchDiv);
    }

    // Time cell on the left
    const timeCell = document.createElement('div');
    timeCell.className = 'grid-cell grid-time-cell';
    const labelStr = defaultSlotLabels[slot - 1] || `Slot ${slot}`;
    timeCell.innerHTML = `<span class="slot-num">Slot ${slot}</span><span>${labelStr}</span>`;
    grid.appendChild(timeCell);

    // Day cells
    constants.days.forEach(day => {
      const cell = document.createElement('div');
      cell.className = 'grid-cell';
      cell.setAttribute('tabindex', '0');
      cell.setAttribute('aria-label', `${day} slot ${slot}`);

      const items = cellMap[day][slot] || [];
      items.forEach(sess => {
        const chip = document.createElement('div');
        const batchClass = `batch-${(sess.batchId || '').toLowerCase()}`;
        const isLab = (sess.courseType === 'LAB');

        chip.className = `session-chip ${batchClass} ${isLab ? 'session-chip-lab' : ''}`;
        chip.innerHTML = `
          <div class="session-chip-top">
            <span class="chip-course-title" title="${sess.courseName}">${sess.courseName}</span>
            <span class="badge-type ${isLab ? 'lab-tag' : 'theory-tag'}">${isLab ? '🔬 LAB' : '📖'}</span>
          </div>
          <div class="session-chip-bottom">
            <span><strong>${sess.batchId}</strong> • ${sess.facultyId}</span>
            <span>📍 ${sess.roomId}</span>
          </div>
        `;
        chip.addEventListener('click', (ev) => {
          ev.stopPropagation();
          openSessionDrawer(sess);
        });
        cell.appendChild(chip);
      });

      grid.appendChild(cell);
    });
  }
}

// ── Side Inspector Drawer ─────────────────────────────────────────────
function initSideDrawer() {
  const drawer = document.getElementById('sessionDrawer');
  const backdrop = document.getElementById('drawerBackdrop');
  const btnClose = document.getElementById('btnCloseDrawer');

  function close() {
    drawer.classList.remove('open');
    drawer.setAttribute('aria-hidden', 'true');
    backdrop.classList.remove('open');
  }

  btnClose.addEventListener('click', close);
  backdrop.addEventListener('click', close);
}

function openSessionDrawer(sess) {
  const drawer = document.getElementById('sessionDrawer');
  const backdrop = document.getElementById('drawerBackdrop');

  document.getElementById('drawerCourseName').textContent = sess.courseName;
  document.getElementById('drawerCourseType').textContent = sess.courseType;
  document.getElementById('drawerCourseType').className =
    `badge ${sess.courseType === 'LAB' ? 'lab-tag' : 'theory-tag'}`;
  document.getElementById('drawerDuration').textContent = `${sess.duration} slot(s) (${sess.duration}h)`;

  document.getElementById('drawerSessionId').textContent = sess.sessionId;
  document.getElementById('drawerCourseId').textContent = sess.courseId;
  document.getElementById('drawerFaculty').textContent = sess.facultyId;
  document.getElementById('drawerBatch').textContent = sess.batchId;
  document.getElementById('drawerBatchStrength').textContent = `${sess.batchStrength} students`;
  document.getElementById('drawerRoom').textContent = sess.roomId || 'Unassigned';
  document.getElementById('drawerRoomCap').textContent = sess.roomCapacity != null ? `${sess.roomCapacity} seats` : 'N/A';

  const capCheck = document.getElementById('drawerCapCheck');
  if (sess.roomCapacity != null) {
    const ok = sess.roomCapacity >= sess.batchStrength;
    capCheck.innerHTML = ok
      ? `<span class="badge badge-feasible">OK (${sess.batchStrength} ≤ ${sess.roomCapacity})</span>`
      : `<span class="badge badge-infeasible">OVERFLOW (${sess.batchStrength} > ${sess.roomCapacity})</span>`;
  } else {
    capCheck.textContent = '—';
  }

  document.getElementById('drawerSlotTime').textContent =
    sess.assigned ? `${sess.day} @ Slot ${sess.startSlot} (${sess.slotLabel})` : 'Unassigned';

  drawer.classList.add('open');
  drawer.setAttribute('aria-hidden', 'false');
  backdrop.classList.add('open');
}

// ══════════════════════════════════════════════════════════════════════
// TAB 2: BENCHMARK CHARTS & DATA
// ══════════════════════════════════════════════════════════════════════
async function loadBenchmarks() {
  try {
    const res = await fetch('/api/benchmarks');
    if (!res.ok) throw new Error('Failed to load benchmarks');
    const data = await res.json();
    state.benchData = data;

    // Check if XLARGE has data
    const hasXlarge = data.some(r => r.dataset === 'XLARGE');
    const btnXlarge = document.querySelector('#benchSizeFilters button[data-size="XLARGE"]');
    if (btnXlarge) {
      if (hasXlarge) {
        btnXlarge.classList.remove('btn-disabled');
        btnXlarge.innerHTML = 'XLARGE (322)';
      } else {
        btnXlarge.classList.add('btn-disabled');
      }
    }

    renderBenchmarkCharts();
    renderBenchmarkTable();
  } catch (err) {
    showToast('Failed to load benchmarks: ' + err.message, 'error');
  }
}

function getFilteredBenchData() {
  if (state.benchFilterSize === 'ALL') return state.benchData;
  return state.benchData.filter(r => r.dataset === state.benchFilterSize);
}

function destroyChart(id) {
  if (state.charts[id]) {
    state.charts[id].destroy();
    delete state.charts[id];
  }
}

function renderBenchmarkCharts() {
  if (!window.Chart) return;
  const data = getFilteredBenchData();
  const isDark = state.theme === 'dark';

  const textColor = isDark ? '#94a3b8' : '#475569';
  const gridColor = isDark ? '#233145' : '#e2e8f0';

  // Common chart defaults
  const algoColors = {
    'GREEDY': '#0ea5e9',
    'MRV': '#10b981',
    'BNB': '#f59e0b',
    'PBNB': '#8b5cf6'
  };

  const sizes = ['SMALL', 'MEDIUM', 'LARGE'];
  if (data.some(r => r.dataset === 'XLARGE')) sizes.push('XLARGE');

  const algos = ['GREEDY', 'MRV', 'BNB', 'PBNB'];

  // Helper to build grouped datasets
  function buildDatasets(valFn) {
    return algos.map(algo => ({
      label: algo === 'PBNB' ? 'Parallel B&B (4t)' : (algo === 'BNB' ? 'Sequential B&B' : algo),
      backgroundColor: algoColors[algo],
      borderColor: algoColors[algo],
      borderWidth: 1,
      data: sizes.map(sz => {
        const row = data.find(r => r.dataset === sz && r.algorithm === algo);
        return row ? valFn(row) : null;
      })
    }));
  }

  // 1. Chart Runtime (Log scale)
  destroyChart('chartRuntime');
  const ctxRuntime = document.getElementById('chartRuntime');
  if (ctxRuntime) {
    const dsRuntime = algos.map(algo => ({
      label: algo,
      backgroundColor: algoColors[algo],
      data: sizes.map(sz => {
        const row = data.find(r => r.dataset === sz && r.algorithm === algo);
        return row ? row.runtimeMs : null;
      })
    }));

    state.charts['chartRuntime'] = new Chart(ctxRuntime, {
      type: 'bar',
      data: { labels: sizes, datasets: dsRuntime },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          y: {
            type: 'logarithmic',
            title: { display: true, text: 'Runtime (ms, Log Scale)', color: textColor },
            grid: { color: gridColor },
            ticks: { color: textColor }
          },
          x: { grid: { color: gridColor }, ticks: { color: textColor } }
        },
        plugins: {
          legend: { labels: { color: textColor } },
          tooltip: {
            callbacks: {
              afterLabel: (ctx) => {
                const row = data.find(r => r.dataset === sizes[ctx.dataIndex] && r.algorithm === algos[ctx.datasetIndex]);
                return row && row.timedOut ? '⚠️ Hit Time Limit (Budget Exhausted)' : '';
              }
            }
          }
        }
      }
    });
  }

  // 2. Chart Nodes Explored (Log scale)
  destroyChart('chartNodes');
  const ctxNodes = document.getElementById('chartNodes');
  if (ctxNodes) {
    const dsNodes = ['MRV', 'BNB', 'PBNB'].map(algo => ({
      label: algo,
      backgroundColor: algoColors[algo],
      data: sizes.map(sz => {
        const row = data.find(r => r.dataset === sz && r.algorithm === algo);
        return row ? row.nodes : null;
      })
    }));

    state.charts['chartNodes'] = new Chart(ctxNodes, {
      type: 'bar',
      data: { labels: sizes, datasets: dsNodes },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          y: {
            type: 'logarithmic',
            title: { display: true, text: 'Search Nodes (Log Scale)', color: textColor },
            grid: { color: gridColor },
            ticks: { color: textColor }
          },
          x: { grid: { color: gridColor }, ticks: { color: textColor } }
        },
        plugins: { legend: { labels: { color: textColor } } }
      }
    });
  }

  // 3. Chart Soft Penalty (Lower is better)
  destroyChart('chartPenalty');
  const ctxPenalty = document.getElementById('chartPenalty');
  if (ctxPenalty) {
    const dsPenalty = algos.map(algo => ({
      label: algo,
      backgroundColor: algoColors[algo],
      data: sizes.map(sz => {
        const row = data.find(r => r.dataset === sz && r.algorithm === algo);
        return (row && row.feasible && row.penalty != null) ? row.penalty : null;
      })
    }));

    state.charts['chartPenalty'] = new Chart(ctxPenalty, {
      type: 'bar',
      data: { labels: sizes, datasets: dsPenalty },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          y: {
            title: { display: true, text: 'Soft Penalty (Lower is Better)', color: textColor },
            grid: { color: gridColor },
            ticks: { color: textColor }
          },
          x: { grid: { color: gridColor }, ticks: { color: textColor } }
        },
        plugins: { legend: { labels: { color: textColor } } }
      }
    });
  }

  // 4. Chart Completion Rate (%)
  destroyChart('chartCompletion');
  const ctxCompletion = document.getElementById('chartCompletion');
  if (ctxCompletion) {
    const dsCompletion = algos.map(algo => ({
      label: algo,
      backgroundColor: algoColors[algo],
      data: sizes.map(sz => {
        const row = data.find(r => r.dataset === sz && r.algorithm === algo);
        return row ? Math.round((row.scheduled / row.sessions) * 100) : 0;
      })
    }));

    state.charts['chartCompletion'] = new Chart(ctxCompletion, {
      type: 'bar',
      data: { labels: sizes, datasets: dsCompletion },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          y: {
            max: 100,
            title: { display: true, text: 'Completion (%)', color: textColor },
            grid: { color: gridColor },
            ticks: { color: textColor }
          },
          x: { grid: { color: gridColor }, ticks: { color: textColor } }
        },
        plugins: { legend: { labels: { color: textColor } } }
      }
    });
  }

  // 5. Chart Throughput (nodes/sec) & Throughput Speedup
  destroyChart('chartThroughput');
  const ctxThroughput = document.getElementById('chartThroughput');
  if (ctxThroughput) {
    const bnbThroughputs = sizes.map(sz => {
      const row = data.find(r => r.dataset === sz && r.algorithm === 'BNB');
      return row ? row.nodesPerSec : 0;
    });
    const pbnbThroughputs = sizes.map(sz => {
      const row = data.find(r => r.dataset === sz && r.algorithm === 'PBNB');
      return row ? row.nodesPerSec : 0;
    });

    state.charts['chartThroughput'] = new Chart(ctxThroughput, {
      type: 'bar',
      data: {
        labels: sizes,
        datasets: [
          {
            label: 'Sequential B&B Throughput (nodes/s)',
            backgroundColor: '#f59e0b',
            data: bnbThroughputs
          },
          {
            label: 'Parallel B&B (4t) Throughput (nodes/s)',
            backgroundColor: '#8b5cf6',
            data: pbnbThroughputs
          }
        ]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          y: {
            type: 'logarithmic',
            title: { display: true, text: 'Throughput (nodes/sec, Log Scale)', color: textColor },
            grid: { color: gridColor },
            ticks: { color: textColor }
          },
          x: { grid: { color: gridColor }, ticks: { color: textColor } }
        },
        plugins: {
          legend: { labels: { color: textColor } },
          tooltip: {
            callbacks: {
              afterBody: (items) => {
                const sz = sizes[items[0].dataIndex];
                const seq = data.find(r => r.dataset === sz && r.algorithm === 'BNB');
                const par = data.find(r => r.dataset === sz && r.algorithm === 'PBNB');
                if (seq && par && seq.nodesPerSec > 0 && par.nodesPerSec > 0) {
                  const sp = (par.nodesPerSec / seq.nodesPerSec).toFixed(2);
                  const eff = (sp / 4 * 100).toFixed(1);
                  return `Throughput Speedup: ${sp}x\nEfficiency (4t): ${eff}%\n(Root-splitting node throughput)`;
                }
                return '';
              }
            }
          }
        }
      }
    });
  }
}

// ── Benchmark Table Rendering ─────────────────────────────────────────
function renderBenchmarkTable() {
  const tbody = document.getElementById('benchTableBody');
  const countEl = document.getElementById('benchRowCount');
  const data = getFilteredBenchData();

  countEl.textContent = `${data.length} rows loaded`;
  tbody.innerHTML = '';

  data.forEach(r => {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td><strong>${r.dataset}</strong></td>
      <td><span class="badge ${r.algorithm === 'PBNB' ? 'badge-info' : 'badge-neutral'}">${r.algorithm}</span></td>
      <td>${r.threads}</td>
      <td>${r.scheduled}/${r.sessions}</td>
      <td><span class="badge ${r.feasible ? 'badge-feasible' : 'badge-infeasible'}">${r.feasible ? 'YES' : 'NO'}</span></td>
      <td>${r.timedOut ? '<span class="badge badge-partial">YES</span>' : 'NO'}</td>
      <td>${formatMs(r.runtimeMs)}</td>
      <td>${r.nodes != null ? formatNum(r.nodes) : '—'}</td>
      <td><strong>${r.nodesPerSec != null ? formatNum(r.nodesPerSec) + ' n/s' : '—'}</strong></td>
      <td>${r.backtracks != null ? formatNum(r.backtracks) : '—'}</td>
      <td>${r.pruned != null ? formatNum(r.pruned) : '—'}</td>
      <td>${r.penalty != null ? formatNum(r.penalty) : '—'}</td>
    `;
    tbody.appendChild(tr);
  });
}

function downloadBenchmarkCsv() {
  const data = state.benchData;
  if (!data || data.length === 0) return;

  const headers = ['Dataset', 'Algorithm', 'Threads', 'Courses', 'Sessions', 'Scheduled', 'Feasible', 'TimedOut', 'Runtime_ms', 'Nodes', 'NodesPerSec', 'Backtracks', 'Pruned', 'Penalty'];
  const lines = [headers.join(',')];

  data.forEach(r => {
    lines.push([
      r.dataset,
      r.algorithm,
      r.threads,
      r.courses,
      r.sessions,
      r.scheduled,
      r.feasible ? 'YES' : 'NO',
      r.timedOut ? 'YES' : 'NO',
      r.runtimeMs,
      r.nodes != null ? r.nodes : 'N/A',
      r.nodesPerSec != null ? r.nodesPerSec : 'N/A',
      r.backtracks != null ? r.backtracks : 'N/A',
      r.pruned != null ? r.pruned : 'N/A',
      r.penalty != null ? r.penalty : 'N/A'
    ].join(','));
  });

  const blob = new Blob([lines.join('\n')], { type: 'text/csv;charset=utf-8;' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `benchmark_results_dedup_${Date.now()}.csv`;
  a.click();
  URL.revokeObjectURL(url);
}

// ══════════════════════════════════════════════════════════════════════
// TAB 3: PARALLEL SCALING & AMDAHL'S LAW
// ══════════════════════════════════════════════════════════════════════
async function startScalingSweep() {
  const size = document.getElementById('sweepSize').value;
  const timeLimitMs = parseInt(document.getElementById('sweepTimeLimit').value, 10) || 5000;

  const threadList = [];
  document.querySelectorAll('#sweepThreadChecks input[type="checkbox"]:checked').forEach(cb => {
    threadList.push(parseInt(cb.value, 10));
  });

  if (threadList.length === 0) {
    showToast('Select at least one thread count to test', 'error');
    return;
  }

  const payload = { size, threadList, timeLimitMs };

  try {
    const res = await fetch('/api/sweep', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    if (res.status === 409) {
      showToast('Another solver run or sweep is active!', 'error');
      return;
    }
    if (!res.ok) {
      const err = await res.json();
      throw new Error(err.error || 'Failed to start sweep');
    }
    const data = await res.json();
    state.activeJobId = data.jobId;
    state.jobStartTime = Date.now();

    setSweepRunningUI(true);
    startJobPolling(data.jobId, async (finalJob) => {
      setSweepRunningUI(false);
      if (finalJob.status === 'done' && finalJob.runIds) {
        showToast('Scaling sweep completed successfully!', 'success');
        await loadAndRenderSweepResults(finalJob.runIds);
      } else if (finalJob.status === 'cancelled') {
        showToast('Sweep was cancelled', 'info');
      } else {
        showToast(`Sweep error: ${finalJob.error || 'Execution failed'}`, 'error');
      }
    });

  } catch (err) {
    showToast(err.message, 'error');
  }
}

function setSweepRunningUI(running) {
  const btnStart = document.getElementById('btnStartSweep');
  const btnCancel = document.getElementById('btnCancelSweep');
  const bar = document.getElementById('sweepProgressBar');

  if (running) {
    btnStart.disabled = true;
    btnCancel.style.display = 'inline-flex';
    bar.style.display = 'flex';
  } else {
    btnStart.disabled = false;
    btnCancel.style.display = 'none';
    bar.style.display = 'none';
  }
}

async function loadAndRenderSweepResults(runIds) {
  const runs = [];
  for (const id of runIds) {
    try {
      const res = await fetch(`/api/runs/${id}`);
      if (res.ok) runs.push(await res.json());
    } catch (e) {
      console.warn(e);
    }
  }

  if (runs.length === 0) return;
  renderSweepData(runs);
}

function renderSweepData(runs) {
  const view = document.getElementById('scalingResultsView');
  const empty = document.getElementById('emptyStateSweep');
  view.style.display = 'block';
  empty.style.display = 'none';

  // Sort by threads
  runs.sort((a, b) => a.meta.threads - b.meta.threads);

  // Baseline is threads == 1
  const baseline = runs.find(r => r.meta.threads === 1) || runs[0];
  const baseThroughput = (baseline.metrics.nodes || 1) / ((baseline.metrics.runtimeMs || 1000) / 1000);

  const points = [];
  const tbody = document.getElementById('tblScalingBody');
  tbody.innerHTML = '';

  let maxP = 1;
  let maxSpeedup = 1.0;

  runs.forEach(r => {
    const p = r.meta.threads;
    const nodes = r.metrics.nodes || 0;
    const timeSec = (r.metrics.runtimeMs || 1000) / 1000;
    const throughput = nodes / timeSec;
    const speedup = throughput / baseThroughput;
    const efficiency = speedup / p;

    if (p > maxP) {
      maxP = p;
      maxSpeedup = speedup;
    }

    points.push({ p, nodes, throughput, speedup, efficiency });

    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td><strong>${p} thread(s)</strong></td>
      <td>${formatNum(nodes)}</td>
      <td><strong>${formatNum(Math.round(throughput))} n/s</strong></td>
      <td>${speedup.toFixed(2)}x</td>
      <td><span class="badge ${efficiency >= 0.7 ? 'badge-feasible' : 'badge-neutral'}">${(efficiency * 100).toFixed(1)}%</span></td>
    `;
    tbody.appendChild(tr);
  });

  // Fit Amdahl parallel fraction f
  // f = (1 - 1/S) / (1 - 1/p)
  let f = 0.0;
  let isSuperLinear = false;

  if (maxP > 1) {
    if (maxSpeedup > maxP) {
      isSuperLinear = true;
      f = 1.0;
    } else {
      f = (1.0 - (1.0 / maxSpeedup)) / (1.0 - (1.0 / maxP));
      f = Math.max(0.0, Math.min(1.0, f));
    }
  }

  const badgeAmdahl = document.getElementById('amdahlFitBadge');
  if (isSuperLinear) {
    badgeAmdahl.className = 'badge badge-partial';
    badgeAmdahl.textContent = '⚡ Super-linear: Search-order effects in B&B';
  } else {
    badgeAmdahl.className = 'badge badge-info';
    badgeAmdahl.textContent = `Amdahl Fit: f = ${(f * 100).toFixed(1)}% parallelizable`;
  }

  // Summary notes
  document.getElementById('scalingSummaryNotes').innerHTML = `
    <strong>Analysis:</strong> At ${maxP} threads, throughput speedup reached <strong>${maxSpeedup.toFixed(2)}x</strong>
    with <strong>${((maxSpeedup / maxP) * 100).toFixed(1)}%</strong> parallel efficiency.
    ${isSuperLinear ? 'Parallel B&B experienced super-linear exploration due to early prune-bound synchronization across independent root candidates.' : ''}
  `;

  // Plot Scaling Curve Chart
  destroyChart('chartAmdahl');
  const ctxAmdahl = document.getElementById('chartAmdahl');
  if (ctxAmdahl) {
    const labels = points.map(pt => `${pt.p} Threads`);
    const threadVals = points.map(pt => pt.p);
    const measuredVals = points.map(pt => pt.speedup);
    const idealVals = points.map(pt => pt.p); // y = p

    // Amdahl curve: S(p) = 1 / ((1-f) + f/p)
    const amdahlVals = threadVals.map(p => {
      if (f >= 0.999) return p;
      return 1.0 / ((1.0 - f) + (f / p));
    });

    const isDark = state.theme === 'dark';
    const textColor = isDark ? '#94a3b8' : '#475569';
    const gridColor = isDark ? '#233145' : '#e2e8f0';

    state.charts['chartAmdahl'] = new Chart(ctxAmdahl, {
      type: 'line',
      data: {
        labels: labels,
        datasets: [
          {
            label: 'Measured Throughput Speedup S(p)',
            data: measuredVals,
            borderColor: '#0ea5e9',
            backgroundColor: '#0ea5e9',
            borderWidth: 3,
            tension: 0.1,
            pointRadius: 6
          },
          {
            label: 'Ideal Linear Speedup (y = p)',
            data: idealVals,
            borderColor: '#10b981',
            borderDash: [5, 5],
            pointRadius: 0,
            fill: false
          },
          {
            label: `Amdahl Prediction (f = ${(f * 100).toFixed(0)}%)`,
            data: amdahlVals,
            borderColor: '#f59e0b',
            borderDash: [3, 3],
            pointRadius: 0,
            fill: false
          }
        ]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          y: {
            title: { display: true, text: 'Speedup Factor (x)', color: textColor },
            grid: { color: gridColor },
            ticks: { color: textColor }
          },
          x: { grid: { color: gridColor }, ticks: { color: textColor } }
        },
        plugins: { legend: { labels: { color: textColor } } }
      }
    });
  }
}
