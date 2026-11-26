(function () {
  "use strict";

  const announcer = document.getElementById("liveAnnouncer");
  function announce(msg) {
    if (announcer) announcer.textContent = msg;
  }

  // ---- Platform-aware shortcut label ----
  const isMac = typeof navigator !== "undefined" && /Mac|iPhone|iPad/i.test(navigator.platform || navigator.userAgent || "");
  const shortcutKbd = document.getElementById("shortcutKbd");
  if (shortcutKbd) shortcutKbd.textContent = isMac ? "⌘K" : "Ctrl K";

  // ---- 3-Way Geist Theme Engine (system | light | dark) ----
  const mq = window.matchMedia ? window.matchMedia("(prefers-color-scheme: dark)") : null;
  let themeMode = localStorage.getItem("edb-theme-mode") || "system";

  function applyTheme(mode) {
    themeMode = mode;
    localStorage.setItem("edb-theme-mode", mode);
    const resolved = mode === "system" ? (mq && mq.matches ? "dark" : "light") : mode;
    document.documentElement.setAttribute("data-theme", resolved);
    document.querySelectorAll("[data-theme-choice]").forEach((btn) => {
      btn.setAttribute("aria-pressed", String(btn.getAttribute("data-theme-choice") === mode));
    });
  }

  applyTheme(themeMode);
  if (mq && mq.addEventListener) {
    mq.addEventListener("change", () => {
      if (themeMode === "system") applyTheme("system");
    });
  }
  document.querySelectorAll("[data-theme-choice]").forEach((btn) => {
    btn.addEventListener("click", () => applyTheme(btn.getAttribute("data-theme-choice")));
  });

  // ---- Mobile Drawer ----
  const menuBtn = document.getElementById("menuToggle");
  const sidebar = document.getElementById("sidebar");
  if (menuBtn && sidebar) {
    menuBtn.addEventListener("click", () => {
      const open = sidebar.classList.toggle("open");
      menuBtn.setAttribute("aria-expanded", String(open));
    });
    sidebar.querySelectorAll("a").forEach((a) => {
      a.addEventListener("click", () => {
        sidebar.classList.remove("open");
        menuBtn.setAttribute("aria-expanded", "false");
      });
    });
  }

  // ---- Zero-Dependency Syntax Highlighter ----
  function escHtml(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }

  function highlightLine(raw, lang) {
    if (!lang) return escHtml(raw);
    const tokens = [];
    const stash = (cls, text) => {
      const idx = tokens.length;
      tokens.push(`<span class="${cls}">${escHtml(text)}</span>`);
      return `\x00T${idx}\x00`;
    };

    let s = raw;
    if (lang === "sql") {
      s = s.replace(/--.*$/gm, (m) => stash("tok-cmt", m));
      s = s.replace(/'(?:''|[^'])*'/g, (m) => stash("tok-str", m));
      s = s.replace(/\b(CREATE|TABLE|IF|NOT|EXISTS|PRIMARY|KEY|AUTO_INCREMENT|UNIQUE|DEFAULT|INDEX|ON|USING|HASH|BTREE|ALTER|ADD|COLUMN|INSERT|INTO|VALUES|UPDATE|SET|DELETE|FROM|WHERE|SELECT|DISTINCT|INNER|LEFT|RIGHT|CROSS|JOIN|AS|BETWEEN|AND|OR|GROUP|BY|HAVING|ORDER|ASC|DESC|LIMIT|OFFSET|BEGIN|COMMIT|ROLLBACK|PREPARE|EXECUTE|DEALLOCATE|EXPLAIN|SHOW|TABLES|DESCRIBE|CONFIG|LIKE|STATS|CHECKPOINT|INT|REAL|TEXT|BOOL|BLOB|TIMESTAMP|TRUE|FALSE|NULL)\b/gi, (m) => stash("tok-kw", m));
      s = s.replace(/\b(COUNT|SUM|AVG|MIN|MAX|LOWER|UPPER|LENGTH|COALESCE|ABS|NOW|ROUND|SUBSTR)\b/gi, (m) => stash("tok-fn", m));
      s = s.replace(/\b\d+(?:\.\d+)?\b/g, (m) => stash("tok-num", m));
    } else if (lang === "bash") {
      s = s.replace(/#.*$/gm, (m) => stash("tok-cmt", m));
      s = s.replace(/"(?:\\.|[^"])*"|'(?:\\.|[^'])*'/g, (m) => stash("tok-str", m));
      s = s.replace(/\b(curl|elliottdb|python|make|cmake|dumpbin|printf|nc|build\.bat)\b/g, (m) => stash("tok-fn", m));
      s = s.replace(/(^|\s)(--?[a-zA-Z0-9_.-]+)/g, (m, sp, fl) => sp + stash("tok-kw", fl));
    } else if (lang === "python") {
      s = s.replace(/#.*$/gm, (m) => stash("tok-cmt", m));
      s = s.replace(/"(?:\\.|[^"])*"|'(?:\\.|[^'])*'/g, (m) => stash("tok-str", m));
      s = s.replace(/\b(import|def|while|return|str|bytes|len|ord|bytearray)\b/g, (m) => stash("tok-kw", m));
      s = s.replace(/\b\d+\b/g, (m) => stash("tok-num", m));
    } else if (lang === "ini") {
      s = s.replace(/^#.*$/gm, (m) => stash("tok-cmt", m));
      s = s.replace(/^\[[^\]]+\]$/gm, (m) => stash("tok-fn", m));
      s = s.replace(/"(?:\\.|[^"])*"/g, (m) => stash("tok-str", m));
      s = s.replace(/\b(true|false)\b/g, (m) => stash("tok-kw", m));
    }

    s = escHtml(s);
    return s.replace(/\x00T(\d+)\x00/g, (_, i) => tokens[Number(i)]);
  }

  document.querySelectorAll("code[data-lang]").forEach((el) => {
    const lang = el.getAttribute("data-lang");
    if (el.textContent.trim()) {
      el.innerHTML = highlightLine(el.textContent, lang);
    }
  });

  // ---- Copy Helpers ----
  const CHECK_SVG = '<svg width="12" height="12" viewBox="0 0 16 16" fill="none" aria-hidden="true"><path d="M3.5 8.5L6.5 11.5L12.5 4.5" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/></svg>';

  function copyWithFeedback(text, btn) {
    navigator.clipboard.writeText(text).then(() => {
      const prevHtml = btn.innerHTML;
      btn.classList.add("copied");
      btn.innerHTML = `${CHECK_SVG}<span>Copied</span>`;
      announce("Copied to clipboard");
      setTimeout(() => {
        btn.classList.remove("copied");
        btn.innerHTML = prevHtml;
      }, 1300);
    });
  }

  document.querySelectorAll("[data-copy-code]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const codeEl = btn.closest(".code-window")?.querySelector("pre code, pre");
      if (codeEl) copyWithFeedback(codeEl.innerText, btn);
    });
  });

  document.querySelectorAll("[data-copy-target]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const target = document.getElementById(btn.getAttribute("data-copy-target"));
      if (target) copyWithFeedback(target.innerText, btn);
    });
  });

  document.querySelectorAll("[data-copy-url]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const rel = btn.getAttribute("data-copy-url");
      const full = new URL(rel, window.location.href).href;
      copyWithFeedback(full, btn);
    });
  });

  // ---- Accessible Tabbed Switchers ----
  document.querySelectorAll(".tabs").forEach((tabsEl) => {
    const btns = Array.from(tabsEl.querySelectorAll('[role="tab"]'));
    const panels = Array.from(tabsEl.querySelectorAll('[role="tabpanel"]'));
    const activeCopyBtn = tabsEl.querySelector("[data-copy-active-tab]");

    function activateTab(idx) {
      btns.forEach((b, i) => b.setAttribute("aria-selected", String(i === idx)));
      panels.forEach((p, i) => p.classList.toggle("active", i === idx));
    }

    btns.forEach((btn, idx) => {
      btn.addEventListener("click", () => activateTab(idx));
      btn.addEventListener("keydown", (e) => {
        if (e.key === "ArrowRight") {
          e.preventDefault();
          const next = (idx + 1) % btns.length;
          activateTab(next);
          btns[next].focus();
        } else if (e.key === "ArrowLeft") {
          e.preventDefault();
          const prev = (idx - 1 + btns.length) % btns.length;
          activateTab(prev);
          btns[prev].focus();
        }
      });
    });

    if (activeCopyBtn) {
      activeCopyBtn.addEventListener("click", () => {
        const activePanel = tabsEl.querySelector(".tab-panel.active pre");
        if (activePanel) copyWithFeedback(activePanel.innerText, activeCopyBtn);
      });
    }
  });

  // ---- Scroll-Spy ----
  const trackedLinks = Array.from(document.querySelectorAll(".sidebar a[href^='#'], .toc-list a[href^='#']"));
  const sections = Array.from(document.querySelectorAll("section[id], h3[id]"));
  function updateScrollSpy() {
    let current = sections[0] ? sections[0].id : "";
    const y = window.scrollY + 110;
    for (const sec of sections) {
      if (sec.offsetTop <= y) current = sec.id;
    }
    trackedLinks.forEach((a) => {
      a.classList.toggle("active", a.getAttribute("href") === "#" + current);
    });
  }
  window.addEventListener("scroll", updateScrollSpy, { passive: true });
  updateScrollSpy();

  // ---- Interactive 92-Option Config Workbench ----
  const schema = window.EDB_CONFIG_SCHEMA || { sections: [], options: [] };
  const overrides = {};
  let activeSection = "all";
  let mutableOnly = false;
  let cfgQuery = "";

  const pillsEl = document.getElementById("cfgPills");
  const tbodyEl = document.getElementById("cfgTableBody");
  const countEl = document.getElementById("cfgCount");
  const badgeEl = document.getElementById("overrideBadge");
  const previewEl = document.getElementById("cfgPreview");
  const cliPreviewEl = document.getElementById("cfgCliPreview");
  const searchInput = document.getElementById("cfgSearch");
  const clearBtn = document.getElementById("cfgClearBtn");
  const mutableCheck = document.getElementById("cfgMutableOnly");

  function getActiveOverrides() {
    const list = [];
    schema.options.forEach((opt) => {
      if (Object.prototype.hasOwnProperty.call(overrides, opt.full_key) &&
          String(overrides[opt.full_key]) !== String(opt.default_str)) {
        list.push([opt, overrides[opt.full_key]]);
      }
    });
    return list;
  }

  function buildConfText() {
    const active = getActiveOverrides();
    if (active.length === 0) {
      return "# Default configuration (0 overrides). Edit any value above or pick a preset.\n";
    }
    const bySec = {};
    active.forEach(([opt, val]) => {
      (bySec[opt.section] = bySec[opt.section] || []).push([opt, val]);
    });
    const lines = ["# elliottdb.conf", ""];
    Object.keys(bySec).forEach((sec) => {
      lines.push(`[${sec}]`);
      bySec[sec].forEach(([opt, val]) => {
        lines.push(opt.type === "string" ? `${opt.key} = "${val}"` : `${opt.key} = ${val}`);
      });
      lines.push("");
    });
    return lines.join("\n");
  }

  function buildCliFlags() {
    const active = getActiveOverrides();
    if (active.length === 0) return "elliottdb";
    return "elliottdb " + active.map(([opt, val]) => `--${opt.full_key}=${val}`).join(" ");
  }

  function refreshPreviews() {
    const activeCount = getActiveOverrides().length;
    if (badgeEl) {
      badgeEl.textContent = `${activeCount} override${activeCount === 1 ? "" : "s"}`;
      badgeEl.classList.toggle("has-changes", activeCount > 0);
    }
    if (previewEl) previewEl.innerHTML = highlightLine(buildConfText(), "ini");
    if (cliPreviewEl) cliPreviewEl.innerHTML = highlightLine(buildCliFlags(), "bash");
  }

  function renderConfigTable(highlightKey) {
    if (!tbodyEl) return;
    const q = cfgQuery.trim().toLowerCase();
    if (clearBtn) clearBtn.classList.toggle("visible", cfgQuery.length > 0);

    const filtered = schema.options.filter((opt) => {
      if (activeSection !== "all" && opt.section !== activeSection) return false;
      if (mutableOnly && !opt.runtime_mutable) return false;
      if (!q) return true;
      return (
        opt.full_key.toLowerCase().includes(q) ||
        opt.env_var.toLowerCase().includes(q) ||
        opt.description.toLowerCase().includes(q)
      );
    });

    if (countEl) countEl.textContent = `${filtered.length} / ${schema.options.length}`;
    tbodyEl.innerHTML = "";

    filtered.forEach((opt) => {
      const tr = document.createElement("tr");
      tr.id = "cfg-" + opt.full_key.replace(".", "-");
      if (highlightKey && highlightKey === opt.full_key) tr.classList.add("cfg-highlight");

      const hasOverride = Object.prototype.hasOwnProperty.call(overrides, opt.full_key) &&
        String(overrides[opt.full_key]) !== String(opt.default_str);
      const curVal = hasOverride ? overrides[opt.full_key] : opt.default_str;
      const rangeText = opt.type === "int" ? `${opt.min}..${opt.max}` : opt.type;

      tr.innerHTML = `
        <td><code>${escHtml(opt.full_key)}</code><br><span style="font-size:11px;color:var(--ds-text-dim);font-family:var(--font-mono)">${escHtml(opt.env_var)}</span></td>
        <td><input class="cfg-val-input${hasOverride ? " modified" : ""}" data-key="${escHtml(opt.full_key)}" value="${escHtml(String(curVal))}" aria-label="${escHtml(opt.full_key)}"></td>
        <td><code>${escHtml(rangeText)}</code></td>
        <td><span class="mut-badge ${opt.runtime_mutable ? "mut-yes" : "mut-no"}">${opt.runtime_mutable ? "SET" : "restart"}</span></td>
        <td>${escHtml(opt.description)}</td>
      `;
      const inp = tr.querySelector("input");
      inp.addEventListener("input", () => {
        overrides[opt.full_key] = inp.value;
        inp.classList.toggle("modified", String(inp.value) !== String(opt.default_str));
        refreshPreviews();
      });
      tbodyEl.appendChild(tr);
    });
    refreshPreviews();
  }

  if (pillsEl && schema.sections.length) {
    const allBtn = document.createElement("button");
    allBtn.className = "filter-btn active";
    allBtn.type = "button";
    allBtn.textContent = `all (${schema.options.length})`;
    allBtn.addEventListener("click", () => {
      activeSection = "all";
      pillsEl.querySelectorAll(".filter-btn").forEach((b) => b.classList.remove("active"));
      allBtn.classList.add("active");
      renderConfigTable();
    });
    pillsEl.appendChild(allBtn);

    schema.sections.forEach((sec) => {
      const cnt = schema.options.filter((o) => o.section === sec).length;
      const btn = document.createElement("button");
      btn.className = "filter-btn";
      btn.type = "button";
      btn.textContent = `${sec} (${cnt})`;
      btn.addEventListener("click", () => {
        activeSection = sec;
        pillsEl.querySelectorAll(".filter-btn").forEach((b) => b.classList.remove("active"));
        btn.classList.add("active");
        renderConfigTable();
      });
      pillsEl.appendChild(btn);
    });
  }

  if (searchInput) {
    searchInput.addEventListener("input", () => {
      cfgQuery = searchInput.value;
      renderConfigTable();
    });
  }
  if (clearBtn && searchInput) {
    clearBtn.addEventListener("click", () => {
      cfgQuery = "";
      searchInput.value = "";
      renderConfigTable();
      searchInput.focus();
    });
  }
  if (mutableCheck) {
    mutableCheck.addEventListener("change", () => {
      mutableOnly = mutableCheck.checked;
      renderConfigTable();
    });
  }

  const PRESETS = {
    default: {},
    memory: { "storage.in_memory": "true", "wal.enabled": "false", "query.result_cache_mb": "256", "concurrency.worker_threads": "0" },
    balanced: { "wal.sync_mode": "normal", "wal.checkpoint_interval_sec": "30", "buffer_pool.size_mb": "0", "query.result_cache_enabled": "true" },
    strict: { "wal.sync_mode": "full", "wal.group_commit_ms": "2", "wal.checksum_enabled": "true", "wal.recovery_mode": "strict", "security.auth_enabled": "true" },
    gateway: { "network.max_connections": "0", "network.max_pipeline_depth": "4096", "concurrency.queue_capacity": "262144", "concurrency.conn_slab_chunk": "4096", "network.idle_timeout_sec": "600" }
  };

  const presetBtns = document.querySelectorAll("[data-preset]");
  presetBtns.forEach((btn) => {
    btn.addEventListener("click", () => {
      const pName = btn.getAttribute("data-preset");
      const p = PRESETS[pName] || {};
      presetBtns.forEach((b) => b.classList.remove("active"));
      btn.classList.add("active");
      Object.keys(overrides).forEach((k) => delete overrides[k]);
      Object.assign(overrides, p);
      renderConfigTable();
    });
  });

  const dlBtn = document.getElementById("downloadConfBtn");
  if (dlBtn) {
    dlBtn.addEventListener("click", () => {
      const blob = new Blob([buildConfText()], { type: "text/plain" });
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = "elliottdb.conf";
      a.click();
    });
  }

  const copyConfBtn = document.getElementById("copyConfBtn");
  if (copyConfBtn) copyConfBtn.addEventListener("click", () => copyWithFeedback(buildConfText(), copyConfBtn));
  const copyCliBtn = document.getElementById("copyCliBtn");
  if (copyCliBtn) copyCliBtn.addEventListener("click", () => copyWithFeedback(buildCliFlags(), copyCliBtn));

  renderConfigTable();

  // ---- Keyboard-Navigable Command Palette (⌘K / Ctrl+K) ----
  const modal = document.getElementById("searchModal");
  const modalInput = document.getElementById("modalSearchInput");
  const modalResults = document.getElementById("modalSearchResults");
  const openSearchBtn = document.getElementById("openSearchBtn");
  let activeResultIdx = 0;
  let lastFocusEl = null;

  const docIndex = Array.from(document.querySelectorAll("section[id], h3[id]")).map((el) => ({
    kind: "Sections",
    title: el.dataset.title || el.querySelector("h2, h1")?.textContent || el.textContent.trim(),
    href: "#" + el.id,
    desc: el.dataset.desc || "Documentation section"
  }));

  schema.options.forEach((opt) => {
    docIndex.push({
      kind: "Configuration",
      title: opt.full_key,
      fullKey: opt.full_key,
      href: "#config",
      desc: `[${opt.type}, default=${opt.default_str}] ${opt.description}`
    });
  });

  function updateActiveResult(items) {
    items.forEach((el, idx) => {
      const isAct = idx === activeResultIdx;
      el.classList.toggle("active", isAct);
      el.setAttribute("aria-selected", String(isAct));
      if (isAct) el.scrollIntoView({ block: "nearest" });
    });
  }

  function renderSearch(q) {
    if (!modalResults) return;
    const term = q.trim().toLowerCase();
    const matches = term
      ? docIndex.filter((it) => it.title.toLowerCase().includes(term) || it.desc.toLowerCase().includes(term)).slice(0, 18)
      : docIndex.slice(0, 10);

    modalResults.innerHTML = "";
    activeResultIdx = 0;

    if (matches.length === 0) {
      modalResults.innerHTML = '<div style="padding:14px;color:var(--ds-text-muted);font-size:13px">No matching sections or configuration keys.</div>';
      return;
    }

    const groups = ["Sections", "Configuration"];
    const itemEls = [];
    groups.forEach((grp) => {
      const grpItems = matches.filter((m) => m.kind === grp);
      if (!grpItems.length) return;
      const lbl = document.createElement("div");
      lbl.className = "search-group-label";
      lbl.textContent = grp;
      modalResults.appendChild(lbl);

      grpItems.forEach((m) => {
        const a = document.createElement("a");
        a.className = "search-item";
        a.setAttribute("role", "option");
        a.href = m.href;
        a.innerHTML = `<div class="search-item-title">${escHtml(m.title)}</div><div class="search-item-desc">${escHtml(m.desc)}</div>`;
        const idx = itemEls.length;
        a.addEventListener("mouseenter", () => {
          activeResultIdx = idx;
          updateActiveResult(itemEls);
        });
        a.addEventListener("click", () => {
          closeModal(false);
          if (m.kind === "Configuration" && m.fullKey) {
            activeSection = "all";
            cfgQuery = m.fullKey;
            if (searchInput) searchInput.value = m.fullKey;
            renderConfigTable(m.fullKey);
          }
        });
        itemEls.push(a);
        modalResults.appendChild(a);
      });
    });

    updateActiveResult(itemEls);
  }

  function openModal() {
    if (!modal) return;
    lastFocusEl = document.activeElement;
    modal.classList.add("open");
    if (modalInput) {
      modalInput.value = "";
      renderSearch("");
      setTimeout(() => modalInput.focus(), 15);
    }
  }

  function closeModal(restoreFocus) {
    if (!modal) return;
    modal.classList.remove("open");
    if (restoreFocus && lastFocusEl && typeof lastFocusEl.focus === "function") {
      lastFocusEl.focus();
    }
  }

  if (openSearchBtn) openSearchBtn.addEventListener("click", openModal);
  if (modal) {
    modal.addEventListener("click", (e) => {
      if (e.target === modal) closeModal(true);
    });
  }
  if (modalInput) {
    modalInput.addEventListener("input", () => renderSearch(modalInput.value));
    modalInput.addEventListener("keydown", (e) => {
      const items = Array.from(modalResults.querySelectorAll(".search-item"));
      if (!items.length) return;
      if (e.key === "ArrowDown") {
        e.preventDefault();
        activeResultIdx = (activeResultIdx + 1) % items.length;
        updateActiveResult(items);
      } else if (e.key === "ArrowUp") {
        e.preventDefault();
        activeResultIdx = (activeResultIdx - 1 + items.length) % items.length;
        updateActiveResult(items);
      } else if (e.key === "Enter") {
        e.preventDefault();
        items[activeResultIdx].click();
      }
    });
  }

  document.addEventListener("keydown", (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k") {
      e.preventDefault();
      if (modal && modal.classList.contains("open")) closeModal(true);
      else openModal();
    } else if (e.key === "/" && !["INPUT", "TEXTAREA"].includes(document.activeElement.tagName)) {
      e.preventDefault();
      openModal();
    } else if (e.key === "Escape" && modal && modal.classList.contains("open")) {
      closeModal(true);
    }
  });
})();
