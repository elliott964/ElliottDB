(function () {
  "use strict";

  // ---- Theme Toggle (Light-first editorial manual; honors OS dark preference if unset) ----
  const themeBtn = document.getElementById("themeToggle");
  const prefersDark = window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches;
  const savedTheme = localStorage.getItem("edb-theme") || (prefersDark ? "dark" : "light");
  document.documentElement.setAttribute("data-theme", savedTheme);

  function syncThemeLabel(theme) {
    if (themeBtn) themeBtn.textContent = theme === "dark" ? "Light" : "Dark";
  }
  syncThemeLabel(savedTheme);

  if (themeBtn) {
    themeBtn.addEventListener("click", () => {
      const cur = document.documentElement.getAttribute("data-theme") || "light";
      const next = cur === "dark" ? "light" : "dark";
      document.documentElement.setAttribute("data-theme", next);
      localStorage.setItem("edb-theme", next);
      syncThemeLabel(next);
    });
  }

  // ---- Mobile Menu ----
  const menuBtn = document.getElementById("menuToggle");
  const sidebar = document.getElementById("sidebar");
  if (menuBtn && sidebar) {
    menuBtn.addEventListener("click", () => sidebar.classList.toggle("open"));
    sidebar.querySelectorAll("a").forEach((a) => {
      a.addEventListener("click", () => sidebar.classList.remove("open"));
    });
  }

  // ---- Copy Buttons ----
  function copyText(text, btn) {
    navigator.clipboard.writeText(text).then(() => {
      const old = btn.textContent;
      btn.textContent = "Copied";
      setTimeout(() => { btn.textContent = old; }, 1200);
    });
  }

  document.querySelectorAll(".code-wrap, .tab-panel").forEach((wrap) => {
    const pre = wrap.querySelector("pre");
    if (!pre || wrap.querySelector(".copy-btn")) return;
    const btn = document.createElement("button");
    btn.className = "copy-btn";
    btn.type = "button";
    btn.textContent = "Copy";
    btn.addEventListener("click", () => copyText(pre.innerText, btn));
    wrap.appendChild(btn);
  });

  // ---- Tabbed Code Switchers ----
  document.querySelectorAll(".tabs").forEach((tabsEl) => {
    const btns = tabsEl.querySelectorAll(".tab-btn");
    const panels = tabsEl.querySelectorAll(".tab-panel");
    btns.forEach((btn, idx) => {
      btn.addEventListener("click", () => {
        btns.forEach((b) => b.classList.remove("active"));
        panels.forEach((p) => p.classList.remove("active"));
        btn.classList.add("active");
        if (panels[idx]) panels[idx].classList.add("active");
      });
    });
  });

  // ---- Scroll-Spy ----
  const trackedLinks = Array.from(document.querySelectorAll(".sidebar a[href^='#'], .toc-right a[href^='#']"));
  const sections = Array.from(document.querySelectorAll("section[id], h3[id]"));
  function updateScrollSpy() {
    let current = sections[0] ? sections[0].id : "";
    const y = window.scrollY + 100;
    for (const sec of sections) {
      if (sec.offsetTop <= y) current = sec.id;
    }
    trackedLinks.forEach((a) => {
      a.classList.toggle("active", a.getAttribute("href") === "#" + current);
    });
  }
  window.addEventListener("scroll", updateScrollSpy, { passive: true });
  updateScrollSpy();

  // ---- Interactive 92-Option Config Explorer ----
  const schema = window.EDB_CONFIG_SCHEMA || { sections: [], options: [] };
  const overrides = {};
  let activeSection = "all";
  let mutableOnly = false;
  let cfgQuery = "";

  const pillsEl = document.getElementById("cfgPills");
  const tbodyEl = document.getElementById("cfgTableBody");
  const countEl = document.getElementById("cfgCount");
  const previewEl = document.getElementById("cfgPreview");
  const cliPreviewEl = document.getElementById("cfgCliPreview");
  const searchInput = document.getElementById("cfgSearch");
  const mutableCheck = document.getElementById("cfgMutableOnly");

  function buildConfText() {
    const lines = ["# elliottdb.conf", ""];
    const bySec = {};
    schema.options.forEach((opt) => {
      if (Object.prototype.hasOwnProperty.call(overrides, opt.full_key) &&
          String(overrides[opt.full_key]) !== String(opt.default_str)) {
        (bySec[opt.section] = bySec[opt.section] || []).push([opt, overrides[opt.full_key]]);
      }
    });
    const keys = Object.keys(bySec);
    if (keys.length === 0) {
      return "# Default configuration (no overrides). Edit any row above or choose a preset.\n";
    }
    keys.forEach((sec) => {
      lines.push(`[${sec}]`);
      bySec[sec].forEach(([opt, val]) => {
        lines.push(opt.type === "string" ? `${opt.key} = "${val}"` : `${opt.key} = ${val}`);
      });
      lines.push("");
    });
    return lines.join("\n");
  }

  function buildCliFlags() {
    const flags = [];
    schema.options.forEach((opt) => {
      if (Object.prototype.hasOwnProperty.call(overrides, opt.full_key) &&
          String(overrides[opt.full_key]) !== String(opt.default_str)) {
        flags.push(`--${opt.full_key}=${overrides[opt.full_key]}`);
      }
    });
    return flags.length ? `elliottdb ${flags.join(" ")}` : "elliottdb";
  }

  function refreshPreviews() {
    if (previewEl) previewEl.textContent = buildConfText();
    if (cliPreviewEl) cliPreviewEl.textContent = buildCliFlags();
  }

  function renderConfigTable(highlightKey) {
    if (!tbodyEl) return;
    const q = cfgQuery.trim().toLowerCase();
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

    if (countEl) countEl.textContent = `${filtered.length} / ${schema.options.length} options`;
    tbodyEl.innerHTML = "";

    filtered.forEach((opt) => {
      const tr = document.createElement("tr");
      tr.id = "cfg-" + opt.full_key.replace(".", "-");
      if (highlightKey && highlightKey === opt.full_key) tr.classList.add("cfg-highlight");

      const curVal = Object.prototype.hasOwnProperty.call(overrides, opt.full_key)
        ? overrides[opt.full_key]
        : opt.default_str;
      const rangeText = opt.type === "int" ? `${opt.min}..${opt.max}` : opt.type;

      tr.innerHTML = `
        <td><code>${opt.full_key}</code><br><span style="font-size:11px;color:var(--text-dim);font-family:var(--font-mono)">${opt.env_var}</span></td>
        <td><input class="cfg-val-input" data-key="${opt.full_key}" value="${String(curVal).replace(/"/g, "&quot;")}" aria-label="${opt.full_key}"></td>
        <td><code>${rangeText}</code></td>
        <td><span class="mut-badge ${opt.runtime_mutable ? "mut-yes" : "mut-no"}">${opt.runtime_mutable ? "SET" : "restart"}</span></td>
        <td>${opt.description}</td>
      `;
      const inp = tr.querySelector("input");
      inp.addEventListener("input", () => {
        overrides[opt.full_key] = inp.value;
        refreshPreviews();
      });
      tbodyEl.appendChild(tr);
    });
    refreshPreviews();
  }

  if (pillsEl && schema.sections.length) {
    const allBtn = document.createElement("button");
    allBtn.className = "filter-btn active";
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
      if (pName !== "default") btn.classList.add("active");
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
  if (copyConfBtn) copyConfBtn.addEventListener("click", () => copyText(buildConfText(), copyConfBtn));
  const copyCliBtn = document.getElementById("copyCliBtn");
  if (copyCliBtn) copyCliBtn.addEventListener("click", () => copyText(buildCliFlags(), copyCliBtn));

  renderConfigTable();

  // ---- Ctrl+K Search Modal ----
  const modal = document.getElementById("searchModal");
  const modalInput = document.getElementById("modalSearchInput");
  const modalResults = document.getElementById("modalSearchResults");
  const openSearchBtn = document.getElementById("openSearchBtn");

  const docIndex = Array.from(document.querySelectorAll("section[id], h3[id]")).map((el) => ({
    kind: "Section",
    title: el.dataset.title || el.querySelector("h2, h1")?.textContent || el.textContent.trim(),
    href: "#" + el.id,
    desc: el.dataset.desc || "Documentation section"
  }));

  schema.options.forEach((opt) => {
    docIndex.push({
      kind: "Config",
      title: opt.full_key,
      fullKey: opt.full_key,
      href: "#config",
      desc: `[${opt.type}, default=${opt.default_str}] ${opt.description}`
    });
  });

  function renderSearch(q) {
    if (!modalResults) return;
    const term = q.trim().toLowerCase();
    const matches = term
      ? docIndex.filter((it) => it.title.toLowerCase().includes(term) || it.desc.toLowerCase().includes(term)).slice(0, 18)
      : docIndex.slice(0, 10);

    modalResults.innerHTML = matches.length
      ? ""
      : '<div style="padding:12px;color:var(--text-muted)">No matching sections or configuration keys.</div>';

    matches.forEach((m) => {
      const a = document.createElement("a");
      a.className = "search-item";
      a.href = m.href;
      a.innerHTML = `<div class="search-item-meta">${m.kind}</div><div><strong>${m.title}</strong></div><div class="search-item-desc">${m.desc}</div>`;
      a.addEventListener("click", () => {
        modal.classList.remove("open");
        if (m.kind === "Config" && m.fullKey) {
          activeSection = "all";
          cfgQuery = m.fullKey;
          if (searchInput) searchInput.value = m.fullKey;
          renderConfigTable(m.fullKey);
        }
      });
      modalResults.appendChild(a);
    });
  }

  function openModal() {
    if (!modal) return;
    modal.classList.add("open");
    if (modalInput) {
      modalInput.value = "";
      renderSearch("");
      setTimeout(() => modalInput.focus(), 20);
    }
  }

  if (openSearchBtn) openSearchBtn.addEventListener("click", openModal);
  if (modal) {
    modal.addEventListener("click", (e) => {
      if (e.target === modal) modal.classList.remove("open");
    });
  }
  if (modalInput) {
    modalInput.addEventListener("input", () => renderSearch(modalInput.value));
  }

  document.addEventListener("keydown", (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k") {
      e.preventDefault();
      openModal();
    } else if (e.key === "/" && document.activeElement.tagName !== "INPUT" && document.activeElement.tagName !== "TEXTAREA") {
      e.preventDefault();
      openModal();
    } else if (e.key === "Escape" && modal && modal.classList.contains("open")) {
      modal.classList.remove("open");
    }
  });
})();
