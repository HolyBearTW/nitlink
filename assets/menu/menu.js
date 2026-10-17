    // Localization resources are external so a new language can be added
    // without rewriting the UI. Missing keys always resolve through en-US.
    const localeResources = window.NitLinkLocales || {};
    const englishLocale = localeResources['en-US'] || {};
    const hasWebViewHost = () => !!(window.chrome && window.chrome.webview &&
      window.chrome.webview.postMessage);
    const detectStandaloneLocale = () => {
      const locales = (window.navigator && window.navigator.languages && window.navigator.languages.length)
        ? window.navigator.languages
        : [window.navigator && window.navigator.language];
      for (const locale of locales) {
        if (typeof locale !== 'string') continue;
        const tag = locale.toLowerCase();
        if (['zh-cn', 'zh-sg', 'zh-hans'].includes(tag) || tag.startsWith('zh-hans-')) return 'zh-CN';
        if (['zh-tw', 'zh-hant-tw'].includes(tag)) return 'zh-TW';
      }
      return 'en-US';
    };
    const standaloneLanguageKey = 'nitlink.languagePreference';
    const standalonePreference = (() => {
      if (hasWebViewHost()) return 'system';
      try {
        const saved = window.localStorage.getItem(standaloneLanguageKey);
        return ['system', 'en-US', 'zh-TW', 'zh-CN'].includes(saved) ? saved : 'system';
      } catch (_) {
        return 'system';
      }
    })();
    let currentLocale = 'en-US';
    let holyBearUpdateState = {};
    let renderHolyBearUpdate = () => {};
    let renderContributorPreview = () => {};
    let renderGameUI = () => {};
    const t = key => {
      const selected = localeResources[currentLocale] || {};
      const value = selected[key] || englishLocale[key];
      return (typeof value === 'string' && value.length > 0) ? value : key;
    };
    // Translate display-only endpoint labels; raw driver diagnostics stay available in logs.
    const endpointLabels = [
      ["exclusive suspended: window inactive, hidden, minimized or regaining focus", "audio.endpoint.0"],
      ["Compatibility shared (Windows conversion)", "audio.endpoint.1"],
      ["Native shared low-period", "audio.endpoint.2"],
      ["Waiting for playback endpoint", "audio.endpoint.3"],
      ["Exclusive event", "audio.endpoint.4"],
      ["driver alignment applied", "audio.endpoint.5"],
      ["driver stream latency", "audio.endpoint.6"],
      ["exclusive fallback:", "audio.endpoint.7"],
      ["exclusive min", "audio.endpoint.8"],
      ["native min", "audio.endpoint.9"],
      ["actual block", "audio.endpoint.10"],
      ["requested", "audio.endpoint.11"],
      ["default", "audio.endpoint.12"],
      ["actual", "audio.endpoint.13"],
      ["capture", "audio.endpoint.14"],
      ["stream", "audio.endpoint.15"],
      ["unknown", "audio.endpoint.16"],
      ["fallback:", "audio.endpoint.17"],
      ["frames", "audio.endpoint.18"]
    ];
    const localizeEndpointInfo = info => endpointLabels.reduce(
      (text, [phrase, key]) => text.split(phrase).join(t(key)), String(info || ""));
    const applyTranslations = locale => {
      currentLocale = localeResources[locale] ? locale : 'en-US';
      document.documentElement.lang = currentLocale;
      document.querySelectorAll('[data-i18n]').forEach(el => {
        el.textContent = t(el.dataset.i18n);
      });
      document.querySelectorAll('[data-i18n-title]').forEach(el => {
        el.title = t(el.dataset.i18nTitle);
      });
      document.querySelectorAll('[data-i18n-aria-label]').forEach(el => {
        el.setAttribute('aria-label', t(el.dataset.i18nAriaLabel));
      });
      document.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
        el.placeholder = t(el.dataset.i18nPlaceholder);
      });
      const active = document.querySelector('.rail-item[data-active="true"]');
      const titleKey = active && ({
        'pane-video': 'top.settings',
        'pane-audio': 'top.settings',
        'pane-games': 'game.pageTitle',
        'pane-shortcuts': 'section.shortcuts',
        'pane-about': 'section.about'
      })[active.dataset.tab];
      const h1 = document.querySelector('.topbar h1');
      if (h1 && titleKey) h1.textContent = t(titleKey);
      const language = document.getElementById('language-select');
      if (language && language.value !== 'system' && language.value !== 'en-US' && language.value !== 'zh-TW' && language.value !== 'zh-CN') {
        language.value = 'system';
      }
      renderHolyBearUpdate();
      renderContributorPreview();
      renderGameUI();
    };
    const localizeValue = value => ({
      Auto: 'value.auto', Stretch: 'value.stretch',
      Right: 'value.right', Left: 'value.left', Full: 'value.full'
    }[value] ? t({
      Auto: 'value.auto', Stretch: 'value.stretch',
      Right: 'value.right', Left: 'value.left', Full: 'value.full'
    }[value]) : value);
    // A real NitLink host will immediately push its effective locale. When
    // this file is opened directly for static testing, use the browser-local
    // preference instead so the language selector still has visible effect.
    const standaloneLocale = standalonePreference === 'system' ? detectStandaloneLocale() : standalonePreference;
    applyTranslations(standaloneLocale);

    // Rail navigation. Every pane stays in the DOM so applyState can keep
    // writing into hidden ones; only the active flag flips.
    (() => {
      const tabs = [...document.querySelectorAll('.rail-item[data-tab]')];
      const titles = {
        'pane-video': 'top.settings', 'pane-audio': 'top.settings',
        'pane-shortcuts': 'section.shortcuts', 'pane-about': 'section.about', 'pane-games': 'game.pageTitle'
      };
      const h1 = document.querySelector('.topbar h1');
      const initiallyActive = tabs.find(tab => tab.dataset.active === 'true');
      if (initiallyActive) document.documentElement.dataset.activePane = initiallyActive.dataset.tab;
      tabs.forEach(tab => tab.addEventListener('click', () => {
        tabs.forEach(other => {
          const pane = document.getElementById(other.dataset.tab);
          const active = (other === tab);
          if (active) other.dataset.active = 'true'; else other.removeAttribute('data-active');
          if (pane) { if (active) pane.dataset.active = 'true'; else pane.removeAttribute('data-active'); }
        });
        document.documentElement.dataset.activePane = tab.dataset.tab;
        if (h1 && titles[tab.dataset.tab]) h1.textContent = t(titles[tab.dataset.tab]);
        if (tab.dataset.tab === 'pane-about') post('markHolyBearUpdateSeen');
      }));
    })();

    const post = (action, value) => {
      const payload = (value === undefined) ? { action } : { action, value };
      if (hasWebViewHost()) {
        window.chrome.webview.postMessage(payload);
      }
    };

    // The WebView never fetches network resources. Native HTTPS checks are opt-in,
    // and this display receives only validated version/status fields.
    renderHolyBearUpdate = () => {
      const s = holyBearUpdateState;
      const button = document.getElementById('holy-bear-check-update');
      if (!button) return;
      const currentTag = typeof s.current === 'string' ? s.current : '';
      const version = currentTag.replace(/^v/, '').replace(/-holybear$/i, '');
      const productTitle = document.getElementById('holy-bear-product-title');
      productTitle.textContent = t('about.productTitle').replace('{version}', version).replace(/\s{2,}/g, ' ').trim();
      const latestTag = typeof s.latest === 'string' ? s.latest : '';
      const lastCheckDate = typeof s.lastCheckUtc === 'number' && s.lastCheckUtc > 0
        ? new Date(s.lastCheckUtc * 1000) : null;
      document.getElementById('holy-bear-last-check').textContent = lastCheckDate && !Number.isNaN(lastCheckDate.getTime())
        ? lastCheckDate.toLocaleString(currentLocale) : t('update.never');
      const toggle = document.getElementById('toggle-holy-bear-auto-update');
      toggle.classList.toggle('on', !!s.automatic);
      toggle.setAttribute('aria-checked', s.automatic ? 'true' : 'false');
      button.disabled = !!s.busy;
      button.setAttribute('aria-busy', s.busy ? 'true' : 'false');
      const states = ['never', 'checking', 'current', 'available', 'offline', 'rateLimited',
        'notFound', 'serverUnavailable', 'apiUnavailable', 'invalid'];
      const key = states.includes(s.status) ? s.status : 'never';
      const status = document.getElementById('holy-bear-update-status');
      status.textContent = t('update.' + key).replace('{version}', latestTag.replace(/^v/, ''));
      status.hidden = key === 'never';
      const link = document.getElementById('holy-bear-release-link');
      const validTag = /^v\d+\.\d+\.\d+(?:\.\d+)?-holybear$/.test(latestTag);
      const expectedUrl = validTag ? 'https://github.com/HolyBearTW/nitlink/releases/tag/' + latestTag : '';
      const showRelease = s.hasNewer === true && key === 'available' && typeof s.url === 'string' && s.url === expectedUrl;
      link.hidden = !showRelease;
      if (showRelease) link.href = expectedUrl;
      else link.removeAttribute('href');
      document.getElementById('holy-bear-update-badge').hidden = !s.unread;
    };
    document.querySelectorAll('.about-details').forEach(details => {
      const summary = details.querySelector('summary');
      const syncExpanded = () => summary.setAttribute('aria-expanded', details.open ? 'true' : 'false');
      syncExpanded();
      details.addEventListener('toggle', syncExpanded);
    });
    const updateButton = document.getElementById('holy-bear-check-update');
    if (updateButton) updateButton.addEventListener('click', () => {
      updateButton.disabled = true;
      const status = document.getElementById('holy-bear-update-status');
      status.textContent = t('update.checking');
      status.hidden = false;
      updateButton.setAttribute('aria-busy', 'true');
      post('checkHolyBearUpdate');
    });
    renderHolyBearUpdate();

    const languageSelect = document.getElementById('language-select');
    if (languageSelect) {
      if (!hasWebViewHost()) languageSelect.value = standalonePreference;
      languageSelect.addEventListener('change', () => {
        const preference = languageSelect.value;
        applyTranslations(preference === 'system' ? detectStandaloneLocale() : preference);
        if (!hasWebViewHost()) {
          try { window.localStorage.setItem(standaloneLanguageKey, preference); } catch (_) {}
        }
        post('setLanguage', preference);
      });
    }

    for (const [id, action] of [['audio-fifo','setAudioFifoMs'], ['audio-render','setAudioRenderMs']]) {
      document.getElementById(id)?.addEventListener('change', e => {
        const n = Number(e.target.value);
        if (!Number.isInteger(n) || n < 3 || n > 100) { e.target.reportValidity(); return; }
        post(action, n);
      });
    }
    document.getElementById('audio-exclusive')?.addEventListener('change', e => post('setAudioExclusive', e.target.checked));
    document.getElementById('audio-drift')?.addEventListener('change', e => post('setAudioDrift', e.target.checked));
    const audioRefreshButton = document.getElementById('audio-refresh');
    let audioRefreshPending = false, audioRefreshTimeout, audioRefreshFeedback;
    let audioRefreshRequest = 0;
    const setAudioRefreshFeedback = (key, busy) => {
      if (!audioRefreshButton) return;
      audioRefreshButton.dataset.i18n = key;
      audioRefreshButton.textContent = t(key);
      audioRefreshButton.disabled = busy;
      audioRefreshButton.setAttribute('aria-busy', String(busy));
    };
    const finishAudioRefresh = success => {
      if (!audioRefreshPending) return;
      audioRefreshPending = false;
      clearTimeout(audioRefreshTimeout);
      setAudioRefreshFeedback(success ? 'audio.refreshed' : 'audio.refreshFailed', false);
      audioRefreshFeedback = setTimeout(() => setAudioRefreshFeedback('audio.refresh', false), 1800);
    };
    audioRefreshButton?.addEventListener('click', () => {
      if (audioRefreshPending) return;
      clearTimeout(audioRefreshFeedback);
      audioRefreshPending = true;
      audioRefreshRequest = audioRefreshRequest === 0xffffffff ? 1 : audioRefreshRequest + 1;
      setAudioRefreshFeedback('audio.refreshing', true);
      if (!hasWebViewHost()) { finishAudioRefresh(false); return; }
      audioRefreshTimeout = setTimeout(() => finishAudioRefresh(false), 5000);
      try { post('refreshAudioDiagnostics', audioRefreshRequest); }
      catch (_) { finishAudioRefresh(false); }
    });

    // Diagnostics are a presentation preference, off at every application start.
    // Hiding the panel never changes audio processing or cumulative counters.
    document.getElementById('audio-debug')?.addEventListener('change', e => {
      document.getElementById('audio-diagnostics').hidden = !e.target.checked;
      e.target.setAttribute('aria-expanded', String(e.target.checked));
    });

    // Contributor cards use only packaged images and fixed profile links.
    (() => {
      const card = document.getElementById('contributor-preview');
      if (!card) return;
      const profiles = {
        HolyBearTW: 'assets/menu/contributors/holybear.png',
        CN115: 'assets/menu/contributors/cn115.jpg',
        n810K: 'assets/menu/contributors/n810k.png'
      };
      const title = document.getElementById('contributor-preview-title');
      const handle = document.getElementById('contributor-preview-handle');
      const avatar = document.getElementById('contributor-preview-avatar');
      const fallback = document.getElementById('contributor-preview-fallback');
      const open = document.getElementById('contributor-preview-open');
      let anchor = null, hoveredLink = null, showTimer, hideTimer;
      const hide = () => {
        clearTimeout(showTimer);
        clearTimeout(hideTimer);
        if (anchor) anchor.setAttribute('aria-expanded', 'false');
        card.hidden = true;
        anchor = null;
      };
      const hideSoon = () => {
        clearTimeout(hideTimer);
        hideTimer = setTimeout(() => {
          const focused = document.activeElement;
          if (!hoveredLink && !card.matches(':hover') && focused !== anchor && !card.contains(focused)) hide();
        }, 160);
      };
      const position = () => {
        if (!anchor || card.hidden) return;
        const rect = anchor.getBoundingClientRect();
        const box = card.getBoundingClientRect();
        const group = anchor.closest('.about-contributors');
        const groupBox = group.getBoundingClientRect();
        const namesRight = Math.max(...Array.from(group.querySelectorAll('.about-contributor > a, .about-contributor > span'),
          element => element.getBoundingClientRect().right));
        const beside = namesRight + box.width + 24 <= window.innerWidth;
        const left = Math.max(12, Math.min(beside ? namesRight + 12 : rect.left, window.innerWidth - box.width - 12));
        const below = groupBox.bottom + 8;
        const top = beside ? rect.top - 11 :
          (below + box.height <= window.innerHeight - 12 ? below : groupBox.top - box.height - 8);
        card.style.left = `${left}px`;
        card.style.top = `${Math.max(12, Math.min(top, window.innerHeight - box.height - 12))}px`;
      };
      renderContributorPreview = () => {
        if (anchor) {
          title.textContent = anchor.textContent.trim();
          position();
        }
      };
      const show = link => {
        clearTimeout(showTimer);
        clearTimeout(hideTimer);
        const username = link.dataset.contributor;
        if (!Object.hasOwn(profiles, username) || link.href !== `https://github.com/${username}` ||
            document.documentElement.dataset.activePane !== 'pane-about' || !link.closest('details').open) return;
        if (anchor && anchor !== link) anchor.setAttribute('aria-expanded', 'false');
        anchor = link;
        anchor.setAttribute('aria-expanded', 'true');
        title.textContent = link.textContent.trim();
        handle.textContent = `@${username}`;
        open.href = link.href;
        fallback.textContent = username.slice(0, 2).toUpperCase();
        fallback.hidden = true;
        avatar.hidden = false;
        avatar.src = profiles[username];
        card.hidden = false;
        position();
      };
      avatar.addEventListener('error', () => { avatar.hidden = true; fallback.hidden = false; });
      document.querySelectorAll('#about-contributors-body a[data-contributor]').forEach(link => {
        link.addEventListener('mouseenter', () => {
          hoveredLink = link;
          clearTimeout(hideTimer);
          clearTimeout(showTimer);
          showTimer = setTimeout(() => show(link), 180);
        });
        link.addEventListener('mouseleave', () => {
          if (hoveredLink === link) hoveredLink = null;
          clearTimeout(showTimer);
          hideSoon();
        });
        link.addEventListener('focus', () => show(link));
        link.addEventListener('blur', hideSoon);
      });
      card.addEventListener('mouseenter', () => clearTimeout(hideTimer));
      card.addEventListener('mouseleave', hideSoon);
      card.addEventListener('focusin', () => clearTimeout(hideTimer));
      card.addEventListener('focusout', hideSoon);
      document.addEventListener('keydown', event => { if (event.key === 'Escape') { hoveredLink = null; hide(); } });
      document.addEventListener('click', event => {
        if (anchor && !anchor.contains(event.target) && !card.contains(event.target)) hide();
      });
      document.addEventListener('scroll', () => { hoveredLink = null; hide(); }, true);
      document.getElementById('about-contributors').addEventListener('toggle', event => { if (!event.target.open) hide(); });
      window.addEventListener('resize', hide);
      window.addEventListener('blur', hide);
      document.addEventListener('visibilitychange', () => { if (document.hidden) hide(); });
    })();

    // Small help buttons support pointer hover, keyboard focus and touch/click.
    // A fixed tooltip stays inside the viewport even in the narrow side panel.
    (() => {
      const tip = document.getElementById('audio-tooltip');
      let anchor = null, hideTimer;
      const hide = () => { clearTimeout(hideTimer); tip.hidden = true; anchor = null; };
      const hideSoon = () => { clearTimeout(hideTimer); hideTimer = setTimeout(hide, 150); };
      const show = button => {
        clearTimeout(hideTimer);
        anchor = button;
        tip.textContent = t(button.dataset.audioHelp);
        tip.hidden = false;
        const rect = button.getBoundingClientRect();
        const left = Math.max(12, Math.min(rect.left, window.innerWidth-tip.offsetWidth-12));
        const below = rect.bottom+8;
        const top = below+tip.offsetHeight <= window.innerHeight-12 ? below : Math.max(12,rect.top-tip.offsetHeight-8);
        tip.style.left = `${left}px`; tip.style.top = `${top}px`;
      };
      document.querySelectorAll('[data-audio-help]').forEach(button => {
        button.addEventListener('mouseenter', () => show(button));
        button.addEventListener('mouseleave', e => {
          if (document.activeElement !== button && e.relatedTarget !== tip) hideSoon();
        });
        button.addEventListener('focus', () => show(button));
        button.addEventListener('blur', hide);
        button.addEventListener('click', () => show(button));
      });
      tip.addEventListener('mouseenter', () => clearTimeout(hideTimer));
      tip.addEventListener('mouseleave', () => {
        if (document.activeElement !== anchor) hideSoon();
      });
      document.addEventListener('keydown', e => { if (e.key === 'Escape') hide(); });
      document.addEventListener('click', e => {
        if (anchor && e.target !== anchor && !tip.contains(e.target)) hide();
      });
      document.addEventListener('scroll', () => {
        if (anchor && document.activeElement === anchor) show(anchor); else hide();
      }, true);
      window.addEventListener('resize', hide);
    })();

    document.addEventListener('click', e => {
      let actionEl = e.target.closest('[data-action]');
      if (!actionEl || actionEl.classList.contains('slider')) return;
      const action = actionEl.dataset.action;
      if (actionEl.querySelector('.toggle') || actionEl.classList.contains('toggle')) {
        const toggleEl = actionEl.querySelector('.toggle') || actionEl;
        const willBeOn = !toggleEl.classList.contains('on');
        toggleEl.classList.toggle('on', willBeOn);
        if (toggleEl.getAttribute('role') === 'switch')
          toggleEl.setAttribute('aria-checked', willBeOn ? 'true' : 'false');
        post(action, willBeOn);
        return;
      }
      post(action);
    });

    (() => {
      const slider = document.getElementById('slider-volume');
      const fill = document.getElementById('slider-volume-fill');
      const thumb = document.getElementById('slider-volume-thumb');
      const valEl = document.getElementById('slider-volume-val');
      const setVisual = pct => {
        const clamped = Math.max(0, Math.min(100, pct));
        fill.style.width = clamped + '%';
        thumb.style.left = clamped + '%';
        valEl.textContent = Math.round(clamped);
      };
      const onPointer = e => {
        const rect = slider.getBoundingClientRect();
        const pct = ((e.clientX - rect.left) / rect.width) * 100;
        setVisual(pct);
        post('setVolume', Math.max(0, Math.min(100, pct)) / 100);
      };
      let dragging = false;
      slider.addEventListener('pointerdown', e => { dragging = true; slider.setPointerCapture(e.pointerId); onPointer(e); });
      slider.addEventListener('pointermove', e => { if (dragging) onPointer(e); });
      slider.addEventListener('pointerup', e => { dragging = false; slider.releasePointerCapture(e.pointerId); });
      slider._setFromCpp = setVisual;
    })();

    const opacitySlider = document.getElementById('slider-pip-opacity');
    const opacityValue = document.getElementById('slider-pip-opacity-val');
    const setPiPOpacityVisual = opacity => {
      if (!Number.isFinite(opacity) || opacitySlider.matches(':active')) return;
      const pct = Math.round(Math.max(10, Math.min(100, opacity * 100)));
      opacitySlider.value = pct;
      opacityValue.textContent = pct + '%';
    };
    opacitySlider.addEventListener('input', () => {
      const pct = Number(opacitySlider.value);
      opacityValue.textContent = pct + '%';
      post('setPiPOpacity', pct / 100);
    });

    // Capture source picker + manual format override.
    //
    // The popover hosts two related pieces: a device list (only rendered
    // when more than one capture device is enumerated) and a three-axis
    // manual format override (always rendered). Resolution, Framerate
    // and Format cascade: each drives the option set for the one below
    // it, with "Auto" at index 0 mapping to the empty-value wildcard.
    //
    // State is push-only from C++: applyState forwards the latest
    // devices / active / availableFormats / captureFormatOverride into
    // _updateSource. The popover holds no optimistic state. A failed
    // override (combination unavailable for the live source) reverts
    // visually as soon as the next PushSettingsState arrives with
    // Auto values and the toast notification.
    (() => {
      const trigger = document.getElementById('source-trigger');
      const nameEl = document.getElementById('meta-source');
      const pop = document.getElementById('source-popover');
      const hdmiPicker = document.getElementById('hdmi-source-picker');
      if (!trigger || !nameEl || !pop) return;

      let lastDevices = [];
      let lastActive = '';
      // Available formats from the live capture device, already filtered
      // upstream to skip interlaced entries. Each: { width, height, fps,
      // format }. Empty until the first state push after Open().
      let lastAvailableFormats = [];
      // User's persisted override. Each numeric field at 0 means Auto for
      // that axis; empty format string means Auto for format. All-Auto =
      // full automatic negotiation (the default).
      let lastOverride = {
        width: 0, height: 0, fps: 0,
        fpsNumerator: 0, fpsDenominator: 1, format: ''
      };

      const closePop = () => { pop.dataset.open = 'false'; };

      // Build one labelled <select> row. The Auto option always sits at
      // index 0 with empty value so the cascade's wildcard semantics map
      // directly to falsy values in the change handler; no special
      // casing for "Auto" needed.
      let sourceSelects = {};
      const updateSelect = (sel, options, currentValue) => {
        sel.innerHTML = '';
        [{ value: '', label: t('value.auto') }, ...options].forEach(o => {
          const opt = document.createElement('option');
          opt.value = String(o.value);
          opt.textContent = o.label;
          sel.appendChild(opt);
        });
        sel.value = currentValue;
      };
      const makeSelectRow = (labelKey, options, currentValue, onChange) => {
        const row = document.createElement('div');
        row.className = 'source-popover-select-row';
        const lbl = document.createElement('label');
        lbl.textContent = t(labelKey);
        const sel = document.createElement('select');
        sel.className = 'source-popover-select';

        sourceSelects[labelKey] = sel;
        updateSelect(sel, options, currentValue);

        // Setting .value to a string not present in options falls back to
        // the first option (Auto), which is the correct UX when the
        // persisted override has a dimension that is no longer available
        // (e.g. source changed and the old fps is gone for this res).
        sel.value = currentValue;
        // Block the popover's click-outside-close logic while the user
        // is interacting with the native <select> dropdown. Without
        // this, clicking the control fires the document-level handler
        // below and closes the popover before the user can pick an
        // option.
        sel.addEventListener('click', ev => ev.stopPropagation());
        sel.addEventListener('change', ev => {
          ev.stopPropagation();
          onChange(ev.target.value);
        });
        row.appendChild(lbl);
        row.appendChild(sel);
        (pop._formatContainer || pop).appendChild(row);
      };

      // The renderer supports three pixel formats end-to-end:
      //   P010 (HDR10), NV12 (SDR YUV), BGRA (SDR RGB).
      // YUY2 / UYVY / RGB24 / ARGB32 are exposed by some capture cards
      // (the 4K Pro publishes YUY2 at 1080p exclusively, for example)
      // but routing them through the renderer would require shader
      // paths the renderer does not implement. Filter the cascade
      // strictly to renderable formats so the user is never offered a
      // combination that would fail with a confusing toast.
      const RENDERABLE = new Set(['P010', 'NV12', 'BGRA']);

      // Cascade helpers. Each derives its set from lastAvailableFormats
      // filtered by the upstream override selections AND by renderable-
      // format gating above. Picking Auto on any axis treats that axis
      // as wildcard for the levels below. A resolution / fps that has
      // no renderable format on the live device does not appear in the
      // dropdown at all; the device's full enumeration is still in
      // lastAvailableFormats (the [NitLink/Formats] log shows everything)
      // but the UI is honest about what NitLink can actually deliver.
      const uniqueResolutions = () => {
        const map = new Map();
        lastAvailableFormats.forEach(f => {
          if (!RENDERABLE.has(f.format)) return;
          map.set(`${f.width}x${f.height}`, { w: f.width, h: f.height });
        });
        return [...map.entries()].sort((a, b) => b[1].w - a[1].w);
      };
      const rateKey = (format) => {
        const numerator = Number(format && format.fpsNumerator);
        const denominator = Number(format && format.fpsDenominator);
        if (numerator > 0 && denominator > 0) return `${numerator}/${denominator}`;
        const fps = Number(format && format.fps);
        return fps > 0 ? `${fps}/1` : '0/1';
      };
      const rateLabel = (format) => {
        const numerator = Number(format && format.fpsNumerator);
        const denominator = Number(format && format.fpsDenominator);
        let fps = Number(format && format.fps) || 0;
        if (numerator > 0 && denominator > 0) {
          const exact = numerator / denominator;
          const roundedInteger = Math.round(exact);
          fps = Math.abs(exact - roundedInteger) < 0.01
            ? roundedInteger
            : Number(exact.toFixed(3));
        }
        return `${fps} ${t('units.fps')}`;
      };
      const overrideRateKey = () => {
        const direct = rateKey(lastOverride);
        const legacyFps = Number(lastOverride && lastOverride.fps);
        const legacyNumerator = Number(lastOverride && lastOverride.fpsNumerator);
        if (!(legacyFps > 0) || legacyNumerator > 0) return direct;

        // Integer-only persisted preferences predate rational-rate storage.
        // Resolve their display key against the live native list without
        // mutating lastOverride, so 59 can select 60000/1001 in the picker
        // while the C++ negotiator still owns the authoritative migration.
        const w = Number(lastOverride.width) || 0;
        const h = Number(lastOverride.height) || 0;
        const format = typeof lastOverride.format === 'string'
          ? lastOverride.format : '';
        const candidates = lastAvailableFormats.filter(f =>
          RENDERABLE.has(f.format) &&
          (!w || f.width === w) &&
          (!h || f.height === h) &&
          Number(f.fps) === legacyFps &&
          (!format || f.format === format));
        if (!candidates.length) return direct;

        candidates.sort((a, b) => {
          const aRate = Number(a.fpsNumerator || a.fps) /
            Number(a.fpsDenominator || 1);
          const bRate = Number(b.fpsNumerator || b.fps) /
            Number(b.fpsDenominator || 1);
          return bRate - aRate;
        });
        return rateKey(candidates[0]);
      };
      const uniqueFps = () => {
        // Strict filter: fps options appear only when a resolution is
        // selected AND a renderable format exists at that (resolution,
        // fps) combo on the current device. When Resolution=Auto,
        // returns empty so the user picks resolution first. Prevents
        // the cascade from offering fps that are not achievable at
        // the chosen resolution.
        const set = new Map();
        const w = lastOverride.width, h = lastOverride.height;
        if (!w || !h) return [];
        lastAvailableFormats.forEach(f => {
          if (!RENDERABLE.has(f.format)) return;
          if (f.width === w && f.height === h) set.set(rateKey(f), f);
        });
        return [...set.values()].sort((a, b) =>
          b.fps - a.fps || Number(b.fpsNumerator || b.fps) - Number(a.fpsNumerator || a.fps));
      };
      const uniqueFormats = () => {
        // Strict filter: format options appear only when a resolution
        // is selected. When Resolution=Auto, returns empty so the user
        // picks resolution first.
        const set = new Set();
        const w = lastOverride.width, h = lastOverride.height;
        const selectedRate = overrideRateKey();
        if (!w || !h) return [];
        lastAvailableFormats.forEach(f => {
          if (!RENDERABLE.has(f.format)) return;
          if (f.width === w && f.height === h) {
            if (selectedRate === '0/1' || rateKey(f) === selectedRate) set.add(f.format);
          }
        });
        return [...set];
      };

      // POST the override back to C++. The backend persists into Config
      // and triggers a format reconcile. If the combo is unavailable,
      // the backend falls back to Auto and the next state push carries
      // state.notification = "Capture format unavailable, ..." which
      // surfaces a toast via showToast() in applyState.
      const refreshSourceSelects = () => {
        const resolutionSelected =
          lastOverride.width > 0 && lastOverride.height > 0;
        const choices = {
            'source.resolution': [uniqueResolutions().map(([key]) => ({ value: key, label: key })),
            lastOverride.width ? `${lastOverride.width}x${lastOverride.height}` : ''],
            'source.framerate': [uniqueFps().map(f => ({ value: rateKey(f), label: rateLabel(f) })),
            overrideRateKey() === '0/1' ? '' : overrideRateKey()],
          'source.format': [uniqueFormats().map(f => ({ value: f, label: f })), lastOverride.format],
        };
        Object.entries(choices).forEach(([label, [options, value]]) => {
          const sel = sourceSelects[label];
          // Keep an open native dropdown intact; its dependent controls can
          // still follow the new resolution/FPS without replacing the row.
          if (sel && sel !== document.activeElement) updateSelect(sel, options, value);
        });
        ['source.framerate', 'source.format'].forEach(label => {
          const sel = sourceSelects[label];
          if (sel) sel.disabled = !resolutionSelected;
        });
      };
      const sendOverride = (ov) => {
        lastOverride = ov;
        refreshSourceSelects();
        post('setCaptureFormatOverride', ov);
      };

      const renderPopover = () => {
        // Keep the identity picker mounted with its listeners, selection and
        // custom draft when device/format state refreshes the original menu.
        pop.replaceChildren();
        sourceSelects = {};

        // Device, HDMI identity and the existing format cascade have their
        // own regions. Choosing a game never changes any capture preference.
        pop._formatContainer = null;
        if (lastDevices.length) {
          const sectLabel = document.createElement('div');
          sectLabel.className = 'source-popover-section-label';
          sectLabel.textContent = t('source.captureDevice');
          pop.appendChild(sectLabel);

          lastDevices.forEach(name => {
            const item = document.createElement('div');
            item.className = 'source-popover-item';
            if (name === lastActive) item.dataset.active = 'true';
            const dot = document.createElement('span');
            dot.className = 'source-popover-bullet';
            const lbl = document.createElement('span');
            lbl.className = 'source-popover-name';
            lbl.textContent = name;
            item.appendChild(dot);
            item.appendChild(lbl);
            item.addEventListener('click', ev => {
              ev.stopPropagation();
              post('setPreferredDevice', name);
              closePop();
            });
            pop.appendChild(item);
          });

          const divider = document.createElement('div');
          divider.className = 'source-popover-divider';
          pop.appendChild(divider);
        }

        // Manual capture format. Always rendered so single-device users
        // can still pick a resolution / framerate / format different
        // from auto-negotiation.
        const fmtSectLabel = document.createElement('div');
        fmtSectLabel.className = 'source-popover-section-label';
        fmtSectLabel.textContent = t('game.hdmiSection');
        pop.appendChild(fmtSectLabel);
        pop.appendChild(hdmiPicker);

        const formatDetails = document.createElement('details');
        formatDetails.className = 'source-format-details';
        formatDetails.open = !!(lastOverride.width || lastOverride.fps || lastOverride.format || pop._formatOpen);
        const formatSummary = document.createElement('summary');
        formatSummary.textContent = t('source.manualFormat');
        formatDetails.appendChild(formatSummary);
        formatDetails.addEventListener('toggle', () => { pop._formatOpen = formatDetails.open; });
        pop.appendChild(formatDetails);
        pop._formatContainer = formatDetails;

        const resolutions = uniqueResolutions();
        const currentResValue = lastOverride.width
          ? `${lastOverride.width}x${lastOverride.height}` : '';
        makeSelectRow('source.resolution',
          resolutions.map(([key]) => ({ value: key, label: key })),
          currentResValue,
          (newVal) => {
            const [w, h] = newVal ? newVal.split('x').map(Number) : [0, 0];
            // Resolution=Auto resets the whole override. Partial
            // overrides (native-best resolution with a specific
            // fps/format) are rarely supported by real hardware.
            if (!w || !h) {
              sendOverride({ width: 0, height: 0, fps: 0,
                fpsNumerator: 0, fpsDenominator: 1, format: '' });
              return;
            }
            // Stale-child invalidation: if the persisted fps or format
            // is no longer a valid combination at the newly selected
            // resolution, reset that axis to Auto. Prevents the cascade
            // from sending an override the current device cannot
            // satisfy (e.g. switching from 1080p+240fps to 4K when
            // 4K@240 does not exist).
            const validAtNewRes = lastAvailableFormats.filter(f =>
              RENDERABLE.has(f.format) &&
              f.width === w && f.height === h);
            const selectedRate = overrideRateKey();
            const fpsStillValid = selectedRate !== '0/1' &&
              validAtNewRes.some(f => rateKey(f) === selectedRate);
            const fmtStillValid = lastOverride.format &&
              validAtNewRes.some(f => f.format === lastOverride.format);
            sendOverride({
              width: w, height: h,
              fps: fpsStillValid ? lastOverride.fps : 0,
              fpsNumerator: fpsStillValid ? lastOverride.fpsNumerator : 0,
              fpsDenominator: fpsStillValid ? lastOverride.fpsDenominator : 1,
              format: fmtStillValid ? lastOverride.format : '',
            });
          }
        );

        const fpsValues = uniqueFps();
        makeSelectRow('source.framerate',
          fpsValues.map(f => ({ value: rateKey(f), label: rateLabel(f) })),
          overrideRateKey() === '0/1' ? '' : overrideRateKey(),
          (newVal) => {
            const selected = newVal
              ? lastAvailableFormats.find(f =>
                  f.width === lastOverride.width &&
                  f.height === lastOverride.height &&
                  RENDERABLE.has(f.format) && rateKey(f) === newVal)
              : null;
            const newFps = selected ? selected.fps : 0;
            const newFpsNumerator = selected
              ? Number(selected.fpsNumerator || selected.fps) : 0;
            const newFpsDenominator = selected
              ? Number(selected.fpsDenominator || 1) : 1;
            // Stale-child invalidation: if the persisted format is no
            // longer renderable at (currentResolution, newFps), reset
            // format to Auto. Prevents sending {res, fps, format}
            // where (res, fps, format) is not in availableFormats.
            let format = lastOverride.format;
            if (format) {
              const validAtCombo = lastAvailableFormats.filter(f =>
                RENDERABLE.has(f.format) &&
                f.width === lastOverride.width &&
                f.height === lastOverride.height &&
                (!newVal || rateKey(f) === newVal));
              if (!validAtCombo.some(f => f.format === format)) format = '';
            }
            sendOverride({
              width: lastOverride.width,
              height: lastOverride.height,
              fps: newFps,
              fpsNumerator: newFpsNumerator,
              fpsDenominator: newFpsDenominator,
              format: format,
            });
          }
        );

        const formatValues = uniqueFormats();
        makeSelectRow('source.format',
          formatValues.map(f => ({ value: f, label: f })),
          lastOverride.format,
          (newVal) => {
            sendOverride({
              width: lastOverride.width,
              height: lastOverride.height,
              fps: lastOverride.fps,
              fpsNumerator: lastOverride.fpsNumerator,
              fpsDenominator: lastOverride.fpsDenominator,
              format: newVal,
            });
          }
        );

        const divider = document.createElement('div');
        divider.className = 'source-popover-divider';
        pop.appendChild(divider);

        const refresh = document.createElement('div');
        refresh.className = 'source-popover-action';
        refresh.textContent = t('source.refresh');
        refresh.addEventListener('click', ev => {
          ev.stopPropagation();
          post('refreshDevices');
        });
        pop.appendChild(refresh);
        refreshSourceSelects();
      };

      const openPop = () => {
        renderPopover();
        pop.dataset.open = 'true';
        // Ask the backend for a fresh enumeration so the list reflects
        // anything plugged in since the last state push. The result
        // arrives asynchronously and re-renders the popover in place.
        post('getDeviceList');
      };
      const togglePop = () => {
        (pop.dataset.open === 'true') ? closePop() : openPop();
      };

      trigger.addEventListener('click', e => {
        e.stopPropagation();
        togglePop();
      });
      // The original popover is inside its trigger. Input/editor clicks must
      // not bubble into that trigger and toggle the containing source menu.
      pop.addEventListener('click', e => e.stopPropagation());
      document.addEventListener('click', e => {
        if (!pop.contains(e.target) && !trigger.contains(e.target)) closePop();
      });
      document.addEventListener('keydown', e => {
        if (e.key === 'Escape') closePop();
      });
      pop.addEventListener('focusout', () => {
        queueMicrotask(() => {
          if (pop.dataset.open === 'true') refreshSourceSelects();
        });
      });

      // Public update entry point. applyState pushes the full set on
      // every state refresh; the picker re-renders the popover in
      // place if it's currently open.
      //
      // Re-render is suppressed when focus is inside the popover.
      // PushSettingsState fires every ~1s on the C++ side while the
      // settings menu is visible. Without this guard, the periodic
      // re-render replaces the capture-format children right when a user
      // has the native <select>
      // dropdown open, which closes their dropdown mid-pick. The next
      // un-focused state push will catch the latest data.
      window._updateSource = (devices, active, availableFormats, override) => {
        lastDevices = Array.isArray(devices) ? devices : [];
        lastActive = typeof active === 'string' ? active : '';
        lastAvailableFormats = Array.isArray(availableFormats)
          ? availableFormats : [];
        const incoming = override || { width: 0, height: 0, fps: 0, format: '' };
        // Preserve an explicit zero numerator. Integer-only legacy
        // preferences intentionally use { fps: 59, fpsNumerator: 0 } so the
        // capture negotiator can resolve that display bucket against the
        // card's native rational (for example 60000/1001). Reconstructing
        // 59/1 here would turn an unresolved legacy preference back into the
        // exact-rate bug when the user later changes another Source field.
        const hasIncomingNumerator = Object.prototype.hasOwnProperty.call(
          incoming, 'fpsNumerator');
        const hasIncomingDenominator = Object.prototype.hasOwnProperty.call(
          incoming, 'fpsDenominator');
        lastOverride = {
          width: Number(incoming.width) || 0,
          height: Number(incoming.height) || 0,
          fps: Number(incoming.fps) || 0,
          fpsNumerator: hasIncomingNumerator
            ? (Number(incoming.fpsNumerator) || 0)
            : (Number(incoming.fps) || 0),
          fpsDenominator: hasIncomingDenominator
            ? (Number(incoming.fpsDenominator) || 1)
            : 1,
          format: typeof incoming.format === 'string' ? incoming.format : ''
        };
        nameEl.textContent = lastActive || t('meta.none');
        trigger.dataset.multi = (lastDevices.length > 1) ? 'true' : 'false';
        if (pop.dataset.open === 'true') {
          const interacting = document.activeElement
            && pop.contains(document.activeElement);
          if (!interacting) renderPopover();
          else refreshSourceSelects();
        }
      };
    })();

    // HDMI identity uses its own picker and never requests device enumeration
    // or capture-format changes. Selection is confirmed by the native state.
    (() => {
      const picker = document.getElementById('hdmi-source-picker');
      const trigger = document.getElementById('hdmi-source-trigger');
      const nameEl = document.getElementById('meta-hdmi-source');
      const customRow = document.getElementById('hdmi-source-custom-row');
      const edit = document.getElementById('hdmi-source-custom-edit');
      const editor = document.getElementById('hdmi-source-custom-editor');
      const input = document.getElementById('hdmi-source-custom-input');
      const error = document.getElementById('hdmi-source-custom-error');
      let choices = [];
      let options = [];
      let selected = 'auto';
      let effective = '';
      let custom = '';
      let choicesKey = '';
      const choiceLabel = choice => choice.value === 'auto' ? t('value.auto')
        : choice.value === 'other' ? t('hdmiSource.otherOption') : choice.label;
      const closeEditor = (restoreFocus = false) => {
        editor.hidden = true;
        if (restoreFocus) trigger.focus();
      };
      const renderOptions = () => {
        trigger.replaceChildren();
        options = choices.map(choice => {
          const { value } = choice;
          const option = document.createElement('option');
          option.value = value;
          trigger.appendChild(option);
          return { option, choice };
        });
      };
      const openEditor = () => {
        editor.hidden = false;
        error.hidden = true;
        input.value = custom;
        input.focus();
        input.select();
      };
      // Use the same HTML select and shared CSS as resolution/FPS/format.
      trigger.addEventListener('click', event => event.stopPropagation());
      trigger.addEventListener('change', event => {
        event.stopPropagation();
        const value = trigger.value;
        if (!choices.some(choice => choice.value === value)) {
          trigger.value = selected;
          return;
        }
        post('setManualHdmiSource', value);
        if (value === 'other') openEditor();
        else closeEditor(true);
      });
      edit.addEventListener('click', openEditor);
      picker.addEventListener('keydown', event => {
        if (event.key === 'Escape' && !editor.hidden) {
          event.preventDefault();
          event.stopPropagation();
          closeEditor(true);
        }
      });
      document.getElementById('source-trigger').addEventListener('click', () => closeEditor());
      document.addEventListener('click', event => {
        if (!picker.contains(event.target)) closeEditor();
      });
      const saveCustom = () => {
        const name = input.value.trim();
        if (/[\x00-\x1f\x7f-\x9f\u2028\u2029]/u.test(input.value) ||
            Array.from(name).length > 64) {
          error.hidden = false;
          input.focus();
          return;
        }
        post('setManualHdmiSourceCustom', name);
        closeEditor(true);
      };
      document.getElementById('hdmi-source-custom-save').addEventListener('click', saveCustom);
      document.getElementById('hdmi-source-custom-cancel').addEventListener('click', () => closeEditor(true));
      input.addEventListener('keydown', event => {
        if (event.key === 'Enter') {
          event.preventDefault();
          saveCustom();
        }
      });
      window._updateManualHdmiSource = (manual, sourceLabel, customName, nativeChoices) => {
        // Fixed labels come from the same native mapping as title and Discord.
        if (Array.isArray(nativeChoices)) {
          const valid = nativeChoices.filter(choice => choice &&
            typeof choice.value === 'string' && typeof choice.label === 'string');
          const key = JSON.stringify(valid);
          if (key !== choicesKey) {
            choicesKey = key;
            choices = valid;
            renderOptions();
          }
        }
        trigger.disabled = options.length === 0;
        if (typeof manual === 'string')
          selected = choices.some(choice => choice.value === manual) ? manual : 'auto';
        if (typeof sourceLabel === 'string') effective = sourceLabel;
        if (typeof customName === 'string') custom = customName;
        options.forEach(({ option, choice }) => {
          const label = choiceLabel(choice);
          if (option.textContent !== label) option.textContent = label;
        });
        trigger.value = selected;
        customRow.hidden = selected !== 'other';
        nameEl.textContent = selected === 'auto' ? t('value.auto')
          : effective || t('meta.none');
        trigger.title = effective
          ? `${nameEl.textContent} · ${effective}`
          : t('meta.hdmiSourceInfo');
      };
      window._updateManualHdmiSource('auto', '');
    })();

    // Production game controls. Native HTTPS and fixture hosts use the same
    // bounded bridge actions/events; this code never fetches a remote URL.
    (() => {
      const el = id => document.getElementById(id);
      const query = el('game-query');
      let sequence = 0, searchRequest = 0, coverRequest = 0, debounce, searchTimer, coverTimer, coverImageTimer;
      let current = {}, live = {}, results = [], covers = [], builtIn = [];
      let library = {games:[],favorites:[],recent:[]}, librarySignature = '', renameKey = '';
      let shape = 'all', searchStatus = 'idle', coverStatus = 'idle', notice = '', suggestions = [], queriedTerms = [];
      let coverProvider='all',officialStatus='idle',officialProvider='';
      el('game-choose-source').addEventListener('click',event=>{event.stopPropagation();el('source-trigger').click();el('hdmi-source-trigger').focus();});
      let coverPage = 0, coverHasMore = false, coverTotal = 0, coverView = '', detailsCover = 0;
      const gameCards = new Map(), coverCards = new Map(), favoriteCards = new Map(), recentCards = new Map();
      const updateSearchArtworkGeometry = art => {
        const model=art?._searchModel;
        const provider=String(model?.coverProvider||'');
        const isNintendoOfficial=model?.coverIsOfficial===true&&provider.startsWith('nintendo-');
        if(!art)return;
        if(!isNintendoOfficial){delete art.dataset.artworkProvider;delete art.dataset.artworkOfficial;art.style.removeProperty('--game-art-ratio');return;}
        art.dataset.artworkProvider=provider;art.dataset.artworkOfficial='true';
        const width=Number(art.dataset.width),height=Number(art.dataset.height);
        if(width>0&&height>0)art.style.setProperty('--game-art-ratio',`${width} / ${height}`);
      };
      const image = (art, uri, state = 'empty') => {
        if (!art) return;
        if (uri && art.dataset.uri === uri) return;
        const setStatus = value => {
          art.dataset.imageState=value;
          art.dataset.imageLabel=value==='loading'?t('game.status.loading'):value==='failed'?t('game.imageFailed'):value==='missing'?t('game.thumbnailMissing'):value==='unavailable'?t('game.thumbnailPaused'):'';
          art.title=value==='unavailable'?t('game.thumbnailUnavailable'):art.dataset.imageLabel;
        };
        clearTimeout(art._imageTimer);
        art.replaceChildren(); delete art.dataset.uri;
        delete art.dataset.width;delete art.dataset.height;art.style.removeProperty('--game-art-ratio');
        setStatus(state);
        if (!uri) return;
        // Only same-origin opaque native resources or preview fixture files.
        const native = /^https:\/\/nitlink\.invalid\/game-images\/\d{1,20}\.png$/.test(uri);
        const fixture = !!window.NitLinkPreview && /^preview\/fixtures\/[a-z0-9-]+\.(jpg|png|webp|svg)$/.test(uri);
        const previewApi = !!window.NitLinkPreview && /^preview\/(?:api-images|generated-images)\/[a-f0-9]{64}\.png$/.test(uri);
        if (!native && !fixture && !previewApi && uri!=='assets/nitlink.svg') { setStatus('failed'); return; }
        const img = document.createElement('img'); img.alt = ''; img.decoding = 'async';
        if (art.classList.contains('game-search-art')) img.className = 'game-art-foreground';
        art.dataset.uri = uri; setStatus('loading');
        img.addEventListener('load', () => {
          if (art.dataset.uri !== uri) return;
          clearTimeout(art._imageTimer);
          setStatus('ok');
          art.dataset.width=img.naturalWidth;art.dataset.height=img.naturalHeight;
          // Pixel colors cannot distinguish intentional cover artwork from letterboxing; never crop the source.
          if (art.classList.contains('game-search-art') && img.naturalWidth !== img.naturalHeight) {
            const backdrop = img.cloneNode(false);
            backdrop.className = 'game-art-backdrop';
            backdrop.alt = '';
            backdrop.setAttribute('aria-hidden', 'true');
            art.insertBefore(backdrop, img);
          }
          updateSearchArtworkGeometry(art);
          art.dispatchEvent(new Event('artworkstate'));
        });
        const failed = () => {
          if (art.dataset.uri !== uri) return;
          clearTimeout(art._imageTimer); delete art.dataset.uri;
          img.remove(); setStatus('failed');
          art.dispatchEvent(new Event('artworkstate'));
        };
        img.addEventListener('error', failed);
        art._imageTimer = setTimeout(failed, 12000);
        img.src = uri; art.appendChild(img);
      };
      // Search enrichment and the selected gallery share the native worker.
      // Cancelling one must settle its placeholders, while preserving loaded DOM.
      const settleImages = (models, cards, state = 'unavailable') => {
        let count=0;
        models.forEach(model => {
          const entry = cards.get(model.id);
          if (!model.image && entry?.art.dataset.imageState === 'loading') {
            model.imageStatus = state; image(entry.art, '', state);++count;
          }
        });
        return count;
      };
      const stopSearch = () => {
        clearTimeout(debounce); clearTimeout(searchTimer); searchRequest = ++sequence;
        settleImages(results, gameCards);
        if (searchStatus === 'loading') searchStatus = 'cancelled';
      };
      const stopCovers = () => {
        clearTimeout(coverTimer); clearTimeout(coverImageTimer); coverRequest = ++sequence;
        const pending=settleImages(covers, coverCards);
        if (coverStatus === 'loading'||pending) coverStatus = 'cancelled';
      };
      const renderResults = () => {
        gameCards.clear(); el('game-results').replaceChildren();
        results.forEach(game => {
          const card = document.createElement('button'); card.type = 'button'; card.className = 'game-result';
          card.setAttribute('aria-pressed', String(current.key === `sgdb-${game.id}`));
          const art = document.createElement('div'); art.className = 'game-art game-search-art';
          art._searchModel=game;updateSearchArtworkGeometry(art);
          image(art, game.image, game.imageStatus || 'loading');
          const title = document.createElement('span'); title.className = 'game-result-title';
          title.textContent = game.displayName || game.name;
          const sub = document.createElement('span'); sub.className = 'game-result-subtitle';
          sub.textContent = game.name;
          const origin = document.createElement('span'); origin.className = 'game-result-origin';
          origin.hidden=!window.NitLinkPreview;
          origin.textContent = ['nintendo-tw','playstation-tw'].includes(game.source) ? `${game.source==='playstation-tw'?'Official PlayStation Taiwan':'Official Nintendo Taiwan'} · ${t('game.officialMapping')}` : game.dataMode === 'fixture' || game.source === 'fixture' ? 'FIXTURE' : `REAL API · ${game.source || 'SteamGridDB'}`;
          card.append(art, title, sub, origin);
          card.addEventListener('click', () => {
            const key=`sgdb-${game.id}`,alreadyCurrent=current.key===key;
            stopSearch(); updateStatus();
            post('setGame', key);
            if(alreadyCurrent){shape='all';coverPage=0;stopCovers();renderGameUI();loadCovers();}
          });
          const wrap = document.createElement('div'); wrap.className = 'game-result-wrap';
          const favorite = document.createElement('button'); favorite.type = 'button'; favorite.className = 'game-button game-favorite-action';
          const isFavorite = library.favorites.some(saved=>saved.key===`sgdb-${game.id}`);
          favorite.textContent = t(isFavorite ? 'game.removeFavorite' : 'game.addFavorite');
          favorite.addEventListener('click',()=>post(library.favorites.some(saved=>saved.key===`sgdb-${game.id}`)?'removeGameFavorite':'addGameFavorite',`sgdb-${game.id}`));
          wrap.append(card,favorite); gameCards.set(game.id, {card, art, favorite}); el('game-results').appendChild(wrap);
        });
      };
      const officialCover=cover=>cover.isOfficial===true&&(['nintendo-tw','nintendo-hk-eshop'].includes(cover.provider)||cover.provider==='nintendo-hk-store');
      const coverOrientation=entry=>{
        const width=Number(entry.art.dataset.width),height=Number(entry.art.dataset.height);
        return entry.art.dataset.imageState==='ok'&&width&&height?(width===height?'square':height>width?'portrait':'landscape'):entry.model.shape;
      };
      const refreshCover=entry=>{
        const orientation=coverOrientation(entry);
        entry.tile.hidden=(shape!=='all'&&['square','portrait','landscape'].includes(orientation)&&orientation!==shape)||entry.art.dataset.imageState==='failed';
        entry.tile.dataset.shape=orientation||'';
        entry.card.disabled=entry.art.dataset.imageState!=='ok';
        entry.card.title=entry.art.dataset.imageState==='failed'?t('game.imageFailed'):entry.card.disabled?t('game.status.loading'):t('game.selectCover');
      };
      const showCoverDetails = (cover, open=true) => {
        if(!cover)return;detailsCover=cover.id;
        const entry=coverCards.get(cover.id),art=entry?.art;
        image(el('game-cover-detail-art'),cover.image,cover.imageStatus||'empty');
        const width=Number(art?.dataset.width)||0,height=Number(art?.dataset.height)||0;
        const orientation=width&&height?(width===height?'square':height>width?'portrait':'landscape'):cover.shape;
        const fields=[['game.artworkSource',cover.sourceLabel||'SteamGridDB'],['game.author',cover.author||t('game.authorUnknown')],
          ['game.coverLanguage',cover.languageVerified?t('game.coverLanguage.'+cover.language):t('game.coverLanguage.unknown')],
          ['game.artworkType',t('game.kind.'+(cover.kind||'cover'))],['game.orientation',t('game.'+(['square','portrait','landscape'].includes(orientation)?orientation:'orientationPending'))],
          ['game.previewDimensions',width&&height?`${width} × ${height}`:t('game.orientationPending')]];
        if(cover.region)fields.push(['game.region',cover.region]);
        if(cover.productTitle)fields.push(['game.storeProduct',cover.productTitle]);
        if(cover.productPlatform)fields.push(['game.storePlatform',cover.productPlatform]);
        if(cover.productRelease)fields.push(['game.storeRelease',cover.productRelease]);
        if(cover.crop)fields.push(['game.artworkTreatment',t('game.cropWhitespace')]);
        if(cover.languageSource)fields.push(['game.artworkReference',cover.languageSource]);
        el('game-cover-detail-fields').replaceChildren();
        fields.forEach(([key,value])=>{const term=document.createElement('dt'),description=document.createElement('dd');term.textContent=t(key);description.textContent=value;el('game-cover-detail-fields').append(term,description);});
        el('game-cover-detail-debug').hidden=!window.NitLinkPreview;
        el('game-cover-detail-debug').textContent=window.NitLinkPreview?`${cover.synthetic?t('game.syntheticCover'):cover.dataMode||'real'} · ${cover.provider||'steamgriddb'} · ${cover.id}`:'';
        el('game-detail-use-original').disabled=!entry||entry.card.disabled;el('game-detail-adjust').disabled=!entry||entry.card.disabled;
        if(open&&!el('game-cover-details').open)el('game-cover-details').showModal();
      };
      el('game-cover-details-close').addEventListener('click',()=>el('game-cover-details').close());
      const editCover=cover=>{
        if(!cover?.image)return;
        stopSearch();stopCovers();post('cancelGameSearch');
        let state=current;
        if(cover.id!==current.coverId){post('selectGameCover',{gameId:current.id,coverId:cover.id});state={...current,coverId:cover.id,image:cover.image,sourceImage:cover.image,artworkMode:'original',cropX:.5,cropY:.5,artworkZoom:1};}
        window.NitLinkArtworkEditor?.open(cover,state);
      };
      el('game-adjust-cover').addEventListener('click',()=>editCover({id:current.coverId,image:current.sourceImage||current.image}));
      el('game-detail-use-original').addEventListener('click',()=>{post('selectGameCover',{gameId:current.id,coverId:detailsCover});el('game-cover-details').close();});
      el('game-detail-adjust').addEventListener('click',()=>{const cover=covers.find(cover=>cover.id===detailsCover);el('game-cover-details').close();editCover(cover);});
      const localEffect=document.createElement('p');localEffect.className='game-note';localEffect.hidden=true;
      document.querySelector('.game-discord-preview').before(localEffect);
      const actualArtwork=document.createElement('div');actualArtwork.className='game-discord-original';actualArtwork.hidden=true;
      const actualArt=document.createElement('div');actualArt.className='game-art';actualArt.id='game-discord-original-art';
      const actualText=document.createElement('span');actualText.className='game-note';actualArtwork.append(actualArt,actualText);el('game-discord-image-status').before(actualArtwork);
      const renderCovers = () => {
        const ids=new Set(covers.map(cover=>cover.id));
        for(const [id,entry] of coverCards)if(!ids.has(id)){clearTimeout(entry.art._imageTimer);entry.tile.remove();coverCards.delete(id);}
        el('game-gallery').dataset.shape=shape;
        let officialIndex=0,communityIndex=0;
        covers.forEach(cover=>{
          let entry=coverCards.get(cover.id);
          if(!entry){
            const tile=document.createElement('div');tile.className='game-cover-tile';
            const card=document.createElement('button');card.type='button';card.className='game-cover';
            const art=document.createElement('div');art.className='game-art';card.appendChild(art);
            const badge=document.createElement('span');badge.className='game-cover-badge';
            const kind=document.createElement('span');kind.className='game-cover-badge logo';
            const meta=document.createElement('div');meta.className='game-cover-meta';
            const info=document.createElement('button');info.type='button';info.className='game-cover-info';info.textContent='i';
            const caption=document.createElement('span');caption.className='game-cover-caption';
            entry={tile,card,art,badge,kind,info,caption,meta,model:cover};
            card.addEventListener('click',()=>{if(entry.art.dataset.imageState==='ok')post('selectGameCover',{gameId:current.id,coverId:cover.id});});
            art.addEventListener('artworkstate',()=>{refreshCover(entry);updateStatus();});
            info.addEventListener('click',()=>showCoverDetails(entry.model));meta.append(kind,caption,info);tile.append(card,badge,meta);coverCards.set(cover.id,entry);
          }else if(!cover.image&&entry.model.image){cover.image=entry.model.image;cover.imageStatus=entry.model.imageStatus;}
          entry.model=cover;entry.card.dataset.coverId=String(cover.id);entry.card.dataset.provider=cover.provider||'steamgriddb';entry.card.dataset.kind=cover.kind||'cover';entry.card.setAttribute('aria-pressed',String(current.coverId===cover.id));
          entry.caption.hidden=!cover.productTitle;entry.caption.textContent=[cover.productTitle,cover.productPlatform,cover.productRelease].filter(Boolean).join(' · ');entry.caption.title=entry.caption.textContent;
          entry.card.setAttribute('aria-label',t('game.selectCover')+(officialCover(cover)?' · '+t('game.officialBadge'):''));
          entry.info.setAttribute('aria-label',t('game.artworkDetails'));entry.info.title=t('game.artworkDetails');
          entry.badge.hidden=!officialCover(cover)&&!cover.synthetic;entry.badge.textContent=cover.synthetic?t('game.sampleBadge'):t('game.officialBadge');
          entry.kind.hidden=cover.kind!=='logo'&&cover.kind!=='logoOnly';entry.kind.textContent='Logo';
          image(entry.art,cover.image,cover.imageStatus||'loading');
          refreshCover(entry);
          const container=el(cover.isOfficial?'game-official-gallery':'game-gallery'),index=cover.isOfficial?officialIndex++:communityIndex++;
          if(container.children[index]!==entry.tile)container.insertBefore(entry.tile,container.children[index]||null);
        });
        el('game-official-section').hidden=officialIndex===0;el('game-community-heading').hidden=officialIndex===0||communityIndex===0;
        el('game-official-gallery').dataset.catalogue=String(covers.some(cover=>cover.provider==='nintendo-hk-store'));
        el('game-official-section').dataset.catalogue=el('game-official-gallery').dataset.catalogue;
      };
      const updateStatus = () => {
        const failed = ['offline', 'timeout', 'workerError', 'invalid', 'rateLimited', 'upstreamUnavailable'];
        el('game-search-status').textContent = t(`game.status.${searchStatus}`) + (results.length ? ` · ${t('game.resultCount').replace('{count}',results.length)}` : '');
        el('game-search-status').dataset.error = String(failed.includes(searchStatus));
        el('game-search-status').setAttribute('aria-busy', String(searchStatus === 'loading'));
        el('game-search-empty').hidden = results.length > 0;
        el('game-search-empty').textContent = t(searchStatus === 'idle' ? 'game.searchIdle' : `game.status.${searchStatus}`);
        el('game-search-fallback').hidden = !(['empty', 'partial', ...failed].includes(searchStatus) || suggestions.length);
        const matching=covers.filter(cover=>{const entry=coverCards.get(cover.id),orientation=entry&&coverOrientation(entry);return shape==='all'||!['square','portrait','landscape'].includes(orientation)||orientation===shape;});
        const visible=matching.filter(cover=>coverCards.get(cover.id)?.art.dataset.imageState!=='failed');
        const imageFailures=matching.length-visible.length;
        const officialVisible=visible.some(cover=>cover.isOfficial);
        el('game-official-section').hidden=!officialVisible;
        el('game-community-heading').hidden=!officialVisible||!visible.some(cover=>!cover.isOfficial);
        el('game-cover-status').textContent = t(`game.status.${coverStatus}`) + (visible.length ? ` · ${t('game.coverCount').replace('{count}',visible.length)}` : '')+
          (imageFailures?' · '+t('game.failedCoverCount').replace('{count}',imageFailures):'');
        el('game-query-trace').hidden = !window.NitLinkPreview || !queriedTerms.length;
        el('game-query-trace').textContent = `${t('game.queriedTerms')} · ${queriedTerms.join(' / ')}`;
        if (window.NitLinkPreview) {
          const key = window.NitLinkPreview.mode === 'real' ? 'game.realPreview' : 'game.fixture';
          el('game-preview-mode-note').dataset.i18n = key; el('game-preview-mode-note').textContent = t(key);
        }
        el('game-cover-status').dataset.error = String(failed.includes(coverStatus)||imageFailures>0);
        el('game-cover-empty').hidden = visible.length > 0;
        el('game-cover-empty').textContent = t(imageFailures&&!visible.length?'game.noUsableCovers':covers.length&&!visible.length&&['ok','partial'].includes(coverStatus)?'game.noShapeCovers':coverStatus === 'idle' ? 'game.coverIdle' : `game.status.${coverStatus}`);
        el('game-gallery').setAttribute('aria-busy', String(coverStatus === 'loading'));
        el('game-cover-prev').disabled = !current.id || !coverPage || coverStatus === 'loading';
        el('game-cover-next').disabled = !current.id || !coverHasMore || coverStatus === 'loading';
        el('game-cover-page').textContent = current.id ? t('game.pageCount').replace('{page}',coverPage+1).replace('{total}',coverTotal) : '';
        el('game-cover-pagination').hidden=!coverPage&&!coverHasMore;
        el('game-notice').hidden = !notice;
        el('game-notice').textContent = notice ? t(`game.${notice}`) : '';
        document.querySelectorAll('[data-cover-provider]').forEach(button=>button.setAttribute('aria-pressed',String(button.dataset.coverProvider===coverProvider)));
        document.querySelector('[data-cover-provider="nintendo"]').hidden=!['switch','playstation'].includes(current.artworkPlatform);
        el('game-store-search').hidden=coverProvider!=='nintendo'||current.artworkPlatform!=='switch';
        const storeStatus=['ok','empty','timeout','offline','rateLimited','invalid','upstreamUnavailable','loading'].includes(officialStatus)?officialStatus:officialStatus==='blocked'?'upstreamUnavailable':'idle';
        el('game-store-status').textContent=t(officialProvider==='nintendo-tw'?'game.nintendoTaiwan':officialProvider==='playstation-tw'?'game.playstationTaiwan':'game.hkStore')+' · '+t('game.status.'+storeStatus);
      };
      const loadCovers = () => {
        stopSearch();
        clearTimeout(coverTimer); clearTimeout(coverImageTimer); coverRequest = ++sequence;
        const view=`${current.dataMode||'native'}:${current.key}:${shape}:${coverPage}:${coverProvider}:${el('game-official-query').value.trim()}`;
        if(coverView!==view)covers=covers.filter(cover=>cover.isOfficial&&cover.provider!=='nintendo-hk-store'&&coverProvider!=='community'&&coverView.split(':').slice(0,2).join(':')===view.split(':').slice(0,2).join(':'));
        coverView=view;renderCovers();
        if (!current.id) { coverStatus = 'idle'; updateStatus(); return; }
        coverStatus = 'loading';officialStatus=coverProvider==='community'?'idle':'loading'; updateStatus();
        post('getGameCovers', {gameId: current.id, shape, page: coverPage, request: coverRequest,provider:coverProvider,officialQuery:el('game-official-query').value.trim()});
        const requested = coverRequest;
        coverTimer = setTimeout(() => {
          if (requested === coverRequest) { coverStatus = 'timeout'; settleImages(covers, coverCards, 'failed'); updateStatus(); }
        }, 30000);
      };
      const search = () => {
        stopCovers();
        clearTimeout(debounce); clearTimeout(searchTimer);
        searchRequest = ++sequence; const requested = searchRequest;
        const text = query.value.trim();
        results = []; suggestions = []; queriedTerms = []; el('game-suggestions').replaceChildren(); renderResults(); notice = '';
        if (!text) { searchStatus = 'idle'; post('cancelGameSearch'); updateStatus(); return; }
        searchStatus = 'loading'; updateStatus();
        post('searchGames', {query: text, request: requested});
        searchTimer = setTimeout(() => {
          if (requested === searchRequest) { searchStatus = 'timeout'; updateStatus(); }
        }, 30000); // Primary (12s) plus bounded fallback searches (12s), including bridge delivery.
      };
      query.addEventListener('input', () => {
        clearTimeout(debounce); clearTimeout(searchTimer);
        searchRequest = ++sequence; // Invalidated before the 350ms debounce.
        stopCovers();
        post('cancelGameSearch');
        results = []; renderResults();
        if (!query.value.trim()) { results = []; searchStatus = 'idle'; renderResults(); updateStatus(); return; }
        searchStatus = 'loading'; updateStatus(); debounce = setTimeout(search, 350);
      });
      query.addEventListener('keydown', event => { if (event.key === 'Enter') search(); });
      el('game-search').addEventListener('click', search);
      el('game-other-name').addEventListener('click', () => {
        query.value = current.name || ''; query.focus(); query.select();
      });
      el('game-clear').addEventListener('click', () => {
        stopSearch(); stopCovers(); post('clearGame');
      });
      el('game-auto').addEventListener('click', () => { shape='all';coverPage=0;post('restoreAutoCover');renderGameUI();loadCovers(); });
      el('game-save-quality').addEventListener('click', () => {
        post('saveGameSettings');
      });
      ['square', 'portrait', 'landscape', 'all'].forEach(value => el(`game-shape-${value}`).addEventListener('click', () => {
        shape = value; coverPage = 0; renderGameUI(); loadCovers();
      }));
      el('game-cover-retry').addEventListener('click', () => { coverPage=0;loadCovers(); });
      el('game-cover-prev').addEventListener('click',()=>{if(coverPage){--coverPage;loadCovers();}});
      el('game-cover-next').addEventListener('click',()=>{if(coverHasMore){++coverPage;loadCovers();}});
      document.querySelectorAll('[data-cover-provider]').forEach(button=>button.addEventListener('click',()=>{coverProvider=button.dataset.coverProvider;shape='all';coverPage=0;renderGameUI();loadCovers();}));
      el('game-official-search').addEventListener('click',()=>{coverProvider='nintendo';shape='all';coverPage=0;renderGameUI();loadCovers();});
      el('game-official-query').addEventListener('keydown',event=>{if(event.key==='Enter'){event.preventDefault();el('game-official-search').click();}});
      const beginRename = game => {
        renameKey=game.key;el('game-custom-name').value=game.customName||'';el('game-custom-name').placeholder=game.displayName||game.name;
        el('game-rename-editor').hidden=false;el('game-custom-name').focus();
      };
      el('game-current-rename').addEventListener('click',()=>{const game=library.games.find(game=>game.key===current.key);if(game)beginRename(game);});
      el('game-current-favorite').addEventListener('click',()=>post(library.favorites.some(game=>game.key===current.key)?'removeGameFavorite':'addGameFavorite',current.key));
      el('game-name-save').addEventListener('click',()=>{post('setGameCustomName',{key:renameKey,name:el('game-custom-name').value.trim()});el('game-rename-editor').hidden=true;});
      el('game-name-reset').addEventListener('click',()=>{post('setGameCustomName',{key:renameKey,name:''});el('game-rename-editor').hidden=true;});
      el('game-name-cancel').addEventListener('click',()=>{el('game-rename-editor').hidden=true;});
      ['large','compact'].forEach(size=>el('game-discord-size-'+size).addEventListener('click',()=>{
        document.querySelector('.game-discord-preview').dataset.size=size;
        ['large','compact'].forEach(other=>el('game-discord-size-'+other).setAttribute('aria-pressed',String(other===size)));
      }));
      const renderLibrary = () => {
        el('game-favorites-empty').hidden=library.favorites.length>0;el('game-recent-empty').hidden=library.recent.length>0;
        const title = game => game.customName || game.displayNames?.[currentLocale] || game.displayName || game.name;
        const reconcile = (container, entries, items, create, update) => {
          const keys=new Set(items.map(game=>game.key));
          for(const [key,entry] of entries)if(!keys.has(key)){
            entry.card.querySelectorAll('.game-art').forEach(art=>clearTimeout(art._imageTimer));
            entry.card.remove();entries.delete(key);
          }
          items.forEach((game,index)=>{
            let entry=entries.get(game.key);
            if(!entry){entry=create(game);entries.set(game.key,entry);}
            entry.game=game;update(entry,game);
            if(container.children[index]!==entry.card)container.insertBefore(entry.card,container.children[index]||null);
          });
        };
        reconcile(el('game-favorites'),favoriteCards,library.favorites,game=>{
          const card=document.createElement('article');card.className='game-my-card';card.dataset.key=game.key;
          const select=document.createElement('button');select.type='button';select.className='game-my-select';
          const art=document.createElement('div');art.className='game-art';
          const name=document.createElement('span');name.className='game-my-title';select.append(art,name);select.addEventListener('click',()=>post('setGame',game.key));
          const tools=document.createElement('div');tools.className='game-my-tools';
          const entry={card,art,name,game,buttons:[]};
          [['game.rename',()=>beginRename(entry.game)],['game.removeFavorite',()=>post('removeGameFavorite',game.key)]].forEach(([key,action])=>{
            const button=document.createElement('button');button.type='button';button.className='game-button';button.addEventListener('click',action);tools.appendChild(button);entry.buttons.push({button,key});
          });card.append(select,tools);return entry;
        },(entry,game)=>{
          entry.card.dataset.current=String(game.key===current.key);entry.name.textContent=title(game);image(entry.art,game.image);
          entry.buttons.forEach(({button,key})=>button.textContent=t(key));
        });
        reconcile(el('game-recent'),recentCards,library.recent,game=>{
          const select=document.createElement('button');select.type='button';select.className='game-button game-recent-select';select.dataset.key=game.key;
          const art=document.createElement('div');art.className='game-art';
          const name=document.createElement('span');const tag=document.createElement('span');
          select.append(art,name,tag);select.addEventListener('click',()=>post('setGame',game.key));return {card:select,art,name,tag};
        },(entry,game)=>{
          entry.name.textContent=title(game);entry.tag.textContent=game.favorite?'★':'';image(entry.art,game.image);
        });
      };
      window._updateGameLibrary = value => {
        library={games:value.games||[],favorites:value.favorites||[],recent:value.recent||[]};
        const signature=JSON.stringify(library)+currentLocale;
        if(signature!==librarySignature){librarySignature=signature;renderLibrary();}
        for(const [id,{favorite}] of gameCards)favorite.textContent=t(library.favorites.some(game=>game.key===`sgdb-${id}`)?'game.removeFavorite':'game.addFavorite');
        el('game-current-favorite').textContent=t(library.favorites.some(game=>game.key===current.key)?'game.removeFavorite':'game.addFavorite');
      };
      const elapsed = () => {
        const seconds = Math.max(0, Math.floor(Date.now() / 1000 - (current.startUnix || Date.now() / 1000)));
        el('game-discord-time').textContent = `${t('game.elapsed')} ${String(Math.floor(seconds / 3600)).padStart(2,'0')}:${String(Math.floor(seconds / 60) % 60).padStart(2,'0')}:${String(seconds % 60).padStart(2,'0')}`;
      };
      renderGameUI = () => {
        el('game-current-title').textContent = current.displayName || current.name || t('game.none');
        el('game-current-source').textContent = `${t('game.source')} · ${live.effectiveHdmiSource || t('meta.none')}`;
        el('game-source-guidance').hidden=!!current.artworkPlatform&&current.artworkPlatform!=='unknown';
        el('game-current-discord').textContent = t(current.discordConnected ? 'game.discordConnected' : 'game.discordUnavailable');
        el('game-cover-mode').textContent = current.key ? t(current.manualCover ? 'game.manualMode' : 'game.autoMode') : '';
        image(el('game-current-art'), current.image);
        const edited=current.artworkMode==='fit'||current.artworkMode==='crop';
        const remotePreview=['externalAccepted','externalPending','unavailable','probeAccepted','previewOnly','localEditOnly'].includes(current.discordImageStatus);
        image(el('game-discord-art'), remotePreview&&current.image?current.image:'assets/nitlink.svg');
        el('game-discord-art').title=t(remotePreview?'game.discordArtworkPreview':'game.discordArtworkFallback');
        el('game-current-artwork-tag').hidden=!current.isOfficial;
        el('game-current-artwork-tag').textContent=edited?t(current.isOfficial?'game.derivedOfficial':'game.derivedArtwork'):current.isOfficial?t(current.language==='zh-TW'?'game.officialBadge':'game.officialArtwork')+' · '+(current.provider==='playstation-tw'?t('game.playstationTaiwan'):current.provider==='nintendo-hk-store'?t('game.hkStore'):current.provider==='nintendo-hk-eshop'?'Nintendo eShop CDN':'Nintendo Taiwan'):'';
        el('game-current-artwork-tag').hidden=!current.isOfficial&&!edited;
        localEffect.hidden=!edited;localEffect.textContent=t('game.edit.localEffect');actualArtwork.hidden=!edited;
        if(edited){image(actualArt,['asset','fallback','localImageUnavailable'].includes(current.discordActualImageStatus)?'assets/nitlink.svg':current.sourceImage||'assets/nitlink.svg');actualText.textContent=t('game.edit.discordOriginal')+' · '+t('game.discordImage.'+(current.discordActualImageStatus||'unavailable'));}
        window.NitLinkArtworkEditor?.update(current);
        el('game-adjust-cover').disabled=!current.coverId||!current.image;
        el('game-discord-title').textContent = current.displayName || live.effectiveHdmiSource || t('game.none');
        el('game-discord-name').textContent = current.discordNameStatus === 'accepted' ? live.effectiveHdmiSource || 'NitLink' : 'NitLink';
        el('game-discord-name-status').textContent = t('game.discordName.'+(current.discordNameStatus||'unverified'));
        el('game-discord-signal').textContent = typeof live.discordSignal === 'string' ? live.discordSignal : (live.negotiatedWidth>0&&live.negotiatedHeight>0&&live.negotiatedFps>0
          ? `${live.negotiatedWidth}×${live.negotiatedHeight} · ${live.negotiatedFps} FPS` : '');
        el('game-discord-image-status').textContent = t(`game.discordImage.${current.discordImageStatus || 'unavailable'}`);
        el('game-clear').disabled = !current.key; el('game-save-quality').disabled = !current.key;
        el('game-current-favorite').disabled=!current.key;el('game-current-rename').disabled=!current.key;
        el('game-current-favorite').textContent=t(library.favorites.some(game=>game.key===current.key)?'game.removeFavorite':'game.addFavorite');
        el('game-auto').disabled = !current.id; el('game-cover-retry').disabled = !current.id;
        ['square', 'portrait', 'landscape', 'all'].forEach(value => {
          el(`game-shape-${value}`).setAttribute('aria-pressed', String(shape === value));
          el(`game-shape-${value}`).disabled = !current.id;
        });
        for (const [id, {card}] of gameCards) card.setAttribute('aria-pressed', String(current.key === `sgdb-${id}`));
        for (const [id, {card}] of coverCards) card.setAttribute('aria-pressed', String(current.coverId === id));
        for(const [key,{card}] of favoriteCards)card.dataset.current=String(key===current.key);
        updateStatus(); elapsed();
        const signature=JSON.stringify(library)+currentLocale;
        if(signature!==librarySignature){librarySignature=signature;renderLibrary();}
      };
      window._updateGameState = state => {
        const changed=current.key!==state.key||current.dataMode!==state.dataMode;
        const platformChanged=!!current.artworkPlatform&&current.artworkPlatform!==state.artworkPlatform;
        const localeChanged=!!current.locale&&current.locale!==state.locale;
        if(changed||platformChanged){stopSearch();stopCovers();shape='all';if(platformChanged)coverProvider='all';officialStatus='idle';officialProvider='';const known=results.find(game=>game.id===state.id);const saved=library.games.find(game=>game.key===state.key);el('game-official-query').value=state.officialQuery||saved?.displayNames?.['zh-TW']||known?.displayName||state.name||'';}
        if(platformChanged){results=[];searchStatus='idle';covers=[];renderResults();}
        if (current.locale && current.locale !== state.locale) {
          stopCovers();
          clearTimeout(debounce); clearTimeout(searchTimer); searchRequest = ++sequence;
          results = []; searchStatus = 'idle'; renderResults();
        }
        current = state; renderGameUI();
        if (changed||platformChanged) {
          notice = '';coverPage=0;coverHasMore=false;coverTotal=0;
          loadCovers();
        }else if(localeChanged){renderCovers();if(el('game-cover-details').open)showCoverDetails(covers.find(cover=>cover.id===detailsCover),false);}
      };
      window._updateGameLive = state => {
        live = {...live, ...state};
        if (Array.isArray(state.gameList) && JSON.stringify(builtIn) !== JSON.stringify(state.gameList)) {
          builtIn = state.gameList; el('game-built-in').replaceChildren();
          builtIn.forEach(game => {
            const button = document.createElement('button'); button.type = 'button'; button.className = 'game-button';
            button.textContent = game.title; button.addEventListener('click', () => post('setGame', game.id));
            el('game-built-in').appendChild(button);
          });
        }
        renderGameUI();
      };
      window._gameEvent = event => {
        if (event.locale && event.locale !== currentLocale) return;
        if (event.kind === 'search' && event.request === searchRequest) {
          clearTimeout(searchTimer); searchStatus = event.status;
          const needle=query.value.trim().toLowerCase();
          const relevance=game=>String(game.id)===needle?4:Math.max(...[game.displayName,game.name].map(name=>{
            const text=(name||'').toLowerCase();return text===needle?3:needle&&text.startsWith(needle)?2:needle&&text.includes(needle)?1:0;
          }));
          // Stable relevance groups include provider supplements. Preserve the
          // Worker's order within a group; no popularity scores are invented.
          results = [...event.games||[]].sort((a,b)=>relevance(b)-relevance(a));
          results.forEach(game => { game.dataMode = game.dataMode || event.dataMode || 'real'; });
          suggestions = event.suggestions || []; queriedTerms = event.queriedTerms || [];
          el('game-suggestions').replaceChildren();
          suggestions.forEach(term => {
            const button = document.createElement('button'); button.type = 'button'; button.className = 'game-button'; button.textContent = term;
            button.addEventListener('click', () => { query.value = term; search(); }); el('game-suggestions').appendChild(button);
          });
          renderResults(); updateStatus();
          el('game-search-status').title = Object.entries(event.lookups || {}).map(([source, status]) => `${source}: ${status}`).join(' · ');
        } else if (event.kind === 'covers' && event.request === coverRequest && event.gameId === current.id && event.shape === shape) {
          clearTimeout(coverTimer); coverStatus = event.status; covers = [...event.artworkSources||[],...event.covers||[]];
          officialStatus=event.officialStatus||'idle';
          officialProvider=event.officialProvider||'';
          coverPage=Number(event.page)||0;coverHasMore=event.hasMore===true;coverTotal=Number(event.total)||covers.length;
          covers.forEach(cover => { cover.dataMode = cover.dataMode || event.dataMode || 'real'; });
          renderCovers(); updateStatus();
          clearTimeout(coverImageTimer);
          const requested=coverRequest;
          coverImageTimer=setTimeout(()=>{
            if(requested===coverRequest&&settleImages(covers,coverCards,'failed')){coverStatus='timeout';updateStatus();}
          },45000);
        } else if (event.kind === 'image') {
          const entry = event.coverId ? (event.request === coverRequest && coverCards.get(event.coverId)) :
            (event.request === searchRequest && gameCards.get(event.gameId));
          if (entry) {
            const model=event.coverId?covers.find(cover=>cover.id===event.coverId):results.find(game=>game.id===event.gameId);
            const status=['loading','failed','missing','unavailable'].includes(event.imageStatus)?event.imageStatus:'empty';
            if(model){model.image=event.image;model.imageStatus=status;if(!event.coverId){model.coverProvider=event.coverProvider||'';model.coverShape=event.coverShape||'';model.coverIsOfficial=event.coverIsOfficial===true;}}
            if(!event.coverId){entry.art._searchModel=model;updateSearchArtworkGeometry(entry.art);}
            image(entry.art, event.image, status);
            if(!event.coverId)updateSearchArtworkGeometry(entry.art);
            if(event.coverId){refreshCover(entry);updateStatus();}
            if(model&&event.coverId&&detailsCover===model.id&&el('game-cover-details').open)showCoverDetails(model,false);
          }
        } else if (event.kind === 'currentImage' && event.request === coverRequest && event.gameId === current.id && event.coverId === current.coverId) {
          if (event.imageStatus === 'failed') { notice = 'imageFailed'; updateStatus(); }
        }
      };
      window._gameNotice = key => { notice = key; updateStatus();window.NitLinkArtworkEditor?.notice(key); };
      setInterval(elapsed, 1000);
      renderGameUI();
    })();

    // Transient toast notification. Pushed from C++ via state.notification
    // (single-use, cleared on the C++ side so the same notice never
    // re-fires after dismissal).
    const ensureToast = () => {
      let toast = document.getElementById('toast');
      if (!toast) {
        toast = document.createElement('div');
        toast.id = 'toast';
        toast.className = 'toast';
        document.body.appendChild(toast);
      }
      return toast;
    };

    const showToast = (text) => {
      const toast = ensureToast();
      toast.textContent = text;
      toast.classList.add('toast-show');
      clearTimeout(toast._toastTimer);
      toast._toastTimer = setTimeout(() => {
        toast.classList.remove('toast-show');
      }, 3000);
    };

    // Rich toast for "screenshot saved" events. Shows filename plus a
    // clickable link that posts openScreenshotFolder back to C++, which
    // opens Windows Explorer with the file pre-selected. Auto-dismisses
    // after 3500ms (slightly longer than the plain toast so users have
    // time to read and click).
    const showScreenshotToast = (filepath) => {
      const toast = ensureToast();
      const lastSlash = Math.max(filepath.lastIndexOf('\\'),
                                 filepath.lastIndexOf('/'));
      const filename = lastSlash >= 0
        ? filepath.substring(lastSlash + 1)
        : filepath;
      toast.textContent = '';
      const prefix = document.createElement('span');
      prefix.textContent = t('toast.screenshotSaved');
      const link = document.createElement('span');
      link.textContent = filename;
      link.style.textDecoration = 'underline';
      link.style.cursor = 'pointer';
      link.style.color = 'var(--accent)';
      link.title = t('toast.openFolder');
      link.addEventListener('click', () => {
        post('openScreenshotFolder');
      });
      toast.appendChild(prefix);
      toast.appendChild(link);
      toast.classList.add('toast-show');
      clearTimeout(toast._toastTimer);
      toast._toastTimer = setTimeout(() => {
        toast.classList.remove('toast-show');
      }, 3500);
    };

    const applyState = s => {
      if (!s) return;
      if (s.holyBearUpdate) { holyBearUpdateState = s.holyBearUpdate; renderHolyBearUpdate(); }
      if (s.locale) applyTranslations(s.locale);
      if (languageSelect && s.languagePreference) languageSelect.value = s.languagePreference;
      const setToggle = (id, on) => { const el = document.getElementById(id); if (el) el.classList.toggle('on', !!on); };
      const setText = (id, txt) => { const el = document.getElementById(id); if (el) el.textContent = txt; };
      const setHidden = (id, hidden) => { const el = document.getElementById(id); if (el) el.hidden = hidden; };
      setToggle('toggle-hdr', s.hdrEnabled);
      setToggle('toggle-color', s.colorExpansion);
      setHidden('color-expansion-advanced', s.colorExpansionAvailable !== true);
      setToggle('toggle-nis', s.nisEnabled);
      setToggle('toggle-vsync', s.vsync);
      setToggle('toggle-low-latency', s.lowLatency);
      setToggle('toggle-prevent-sleep', s.preventSleep);
      if (s.presentPacing) {
        setText('pacing-val',
          s.presentPacing === 'unique'   ? t('value.sourceFrameRate') :
          s.presentPacing === 'captured' ? t('value.captureRate')      : t('value.displayRefresh'));
      }
      setToggle('toggle-mute', s.audioMuted);
      for (const [id, key] of [['audio-fifo','audioFifoMs'], ['audio-render','audioRenderMs']]) {
        const input = document.getElementById(id);
        if (input && document.activeElement !== input && Number.isFinite(s[key])) input.value = s[key];
      }
      const exclusiveInput = document.getElementById('audio-exclusive');
      if (exclusiveInput && typeof s.audioExclusive === 'boolean') exclusiveInput.checked = s.audioExclusive;
      const renderInput = document.getElementById('audio-render');
      if (renderInput) renderInput.disabled = s.audioExclusiveActive === true;
      const driftInput = document.getElementById('audio-drift');
      if (driftInput && typeof s.audioDrift === 'boolean') driftInput.checked = s.audioDrift;
      setText('audio-status', s.audioStreaming
        ? `${t('audio.running')} · ${t(s.audioExclusiveActive ? 'audio.exclusiveMode' : 'audio.sharedMode')}`
        : t('audio.waiting'));
      const health = document.getElementById('audio-health');
      if (health && Number.isFinite(s.audioFillMs)) {
        const ms = value => Number.isFinite(value) ? value.toFixed(2) : '--';
        health.textContent = `${s.audioStreaming ? t('audio.running') : t('audio.waiting')} · ${s.audioAdaptive ? 'Sinc SRC' : t('audio.fallback')} · ${s.audioDrift ? t('audio.driftOn') : t('audio.off')}\n` +
          (s.audioExclusiveActive
            ? `${t('audio.exclusiveAuto')} · ${t('audio.block')} ${ms(s.audioEffectiveMs)} ms\nFIFO ${ms(s.audioFillMs)} ms (${t('audio.afterPump')})\n`
            : `${t('audio.requested')} ${ms(s.audioRenderMs)} ms → ${t('audio.effective')} ${ms(s.audioEffectiveMs)} ms\nFIFO ${ms(s.audioFillMs)} ms + ${t('audio.queued')} ${ms(s.audioQueueMs)} ms (${t('audio.afterPump')})\n`) +
          `FIFO ${t('audio.average')} ${ms(s.audioAverageMs)} ms [${ms(s.audioMinMs)}–${ms(s.audioMaxMs)}]\n` +
          `${t('audio.period')} ${ms(s.audioPeriodUs / 1000)} ms · ${t('audio.correction')} ${s.audioPpm} ppm\n` +
          `${t('audio.clockEstimate')} ${s.audioClockValid ? s.audioEstimatedPpm + ' ppm' : t('audio.clockLearning')}\n` +
          `${t('audio.underrunEvents')} ${s.audioUnderrunEvents ?? 0} · ${t('audio.reloads')} ${s.audioReloads ?? 0}\n` +
          `${t('audio.underruns')} ${s.audioUnderruns} · ${t('audio.dropped')} ${s.audioOverruns} · ${t('audio.resyncs')} ${s.audioResyncs}\n` +
          localizeEndpointInfo(s.audioEndpointInfo);
        // Ordinary broadcasts and late replies must not acknowledge this click.
        if (s.audioRefreshRequest === audioRefreshRequest) finishAudioRefresh(true);
      }

      if (typeof s.volume === 'number') {
        const slider = document.getElementById('slider-volume');
        if (slider && slider._setFromCpp) slider._setFromCpp(s.volume * 100);
      }
      setPiPOpacityVisual(s.pipOpacity);
      if (s.aspectRatio) setText('aspect-val', localizeValue(s.aspectRatio));
      if (s.panelSide) {
        setText('panel-side-val', localizeValue(s.panelSide));
        document.documentElement.dataset.side = String(s.panelSide).toLowerCase();
      }
      let configWarning = document.getElementById('config-warning');
      if (!configWarning) {
        configWarning = document.createElement('div');
        configWarning.id = 'config-warning';
        configWarning.setAttribute('role', 'status');
        configWarning.style.cssText = 'flex:none;padding:12px 20px;color:var(--warn);white-space:normal;font-size:13px;line-height:1.5';
        document.querySelector('.content').prepend(configWarning);
      }
      configWarning.textContent = typeof s.configWarning === 'string' ? s.configWarning : '';
      configWarning.hidden = !configWarning.textContent;
      const noSignalMode = s.noSignalMode === 'image' ? 'image' : 'default';
      setText('no-signal-mode-val', t(
        noSignalMode === 'image'
          ? (s.noSignalImage && s.noSignalImageAvailable === false ? 'value.customImageUnavailable' : 'value.customImage')
          : 'value.nitlinkDefault'));
      const noSignalFit = ['contain', 'cover', 'stretch'].includes(s.noSignalFit)
        ? s.noSignalFit : 'contain';
      setText('no-signal-fit-val', t(`value.${noSignalFit}`));
      setText('no-signal-dim-val', s.noSignalDimImage === false
        ? t('value.off') : t('value.on'));
      const customNoSignal = noSignalMode === 'image';
      const path = typeof s.noSignalImage === 'string' ? s.noSignalImage : '';
      setHidden('no-signal-image-row', !customNoSignal);
      setHidden('no-signal-fit-row', !customNoSignal);
      setHidden('no-signal-dim-row', !customNoSignal);
      setHidden('no-signal-remove', !customNoSignal || !path);
      const imageValue = document.getElementById('no-signal-image-value');
      if (imageValue) {
        const slash = Math.max(path.lastIndexOf('\\'), path.lastIndexOf('/'));
        imageValue.textContent = path ? path.substring(slash + 1) : t('meta.none');
        imageValue.title = path || t('meta.none');
      }
      const hasNegotiatedFormat = s.negotiatedWidth > 0 && s.negotiatedHeight > 0;
      const negotiatedFps = s.negotiatedFpsNumerator > 0 && s.negotiatedFpsDenominator > 0
        ? s.negotiatedFpsNumerator / s.negotiatedFpsDenominator : s.negotiatedFps;
      const rateText = negotiatedFps > 0 ? String(Number(negotiatedFps.toFixed(2))) : '--';
      setText('meta-resolution', hasNegotiatedFormat
        ? `${s.negotiatedWidth}x${s.negotiatedHeight} / ${rateText} ${t('units.fps')}`
        : t('meta.none'));
      setText('meta-format', s.negotiatedFormat === 'Unknown' ? t('value.auto') : s.negotiatedFormat);
      setText('meta-link', s.linkText);
      setText('meta-latency', s.latencyText);

      if (window._updateSource) {
        window._updateSource(
          s.captureDevices,
          s.activeDevice,
          s.availableFormats,
          s.captureFormatOverride
        );
      }
      window._updateManualHdmiSource(
        s.manualHdmiSource, s.effectiveHdmiSource, s.manualHdmiSourceCustom, s.manualHdmiSourceOptions);
      window._updateGameLive(s);

      // One-shot user-facing notice from the C++ pipeline. Most state
      // pushes carry an empty string here; non-empty means the C++ side
      // wants the user to see something (currently: format-override
      // fallback). The C++ side already cleared its internal flag when
      // it produced this string, so re-runs of applyState do not
      // re-show stale toasts.
      if (typeof s.notification === 'string' && s.notification.length > 0) {
        showToast(s.notification);
      }

      // Screenshot saved: rich toast with clickable filename that opens
      // the containing folder in Explorer via openScreenshotFolder.
      if (typeof s.screenshotSaved === 'string' && s.screenshotSaved.length > 0) {
        showScreenshotToast(s.screenshotSaved);
      }

      // HDR auto-detect notice: shown when the card cannot read HDR
      // source state (generic Media Foundation devices, for example).
      // Tells the user to toggle HDR manually with Alt+H.
      const hdrNotice = document.getElementById('hdr-autodetect-notice');
      if (hdrNotice) {
        if (s.hdrAutoDetectSupported === false) {
          hdrNotice.removeAttribute('hidden');
        } else {
          hdrNotice.setAttribute('hidden', '');
        }
      }
    };

    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.addEventListener('message', evt => {
        let msg = evt.data;
        if (typeof msg === 'string') try { msg = JSON.parse(msg); } catch { return; }
        if (msg && msg.state) {
          try {
            applyState(msg.state);
          } catch (error) {
            const detail = String(error).replace(/[\u0000-\u001f\u007f]/g, ' ').slice(0, 512);
            post('javascriptException', detail || 'applyState failed');
          }
        }
        if (msg && typeof msg.toast === 'string') showToast(msg.toast);
        if (msg && msg.gameState) window._updateGameState(msg.gameState);
        if (msg && msg.gameLibrary) window._updateGameLibrary(msg.gameLibrary);
        if (msg && msg.gameEvent) window._gameEvent(msg.gameEvent);
        if (msg && msg.gameNotice) window._gameNotice(msg.gameNotice);
        if (msg && typeof msg.pipOpacity === 'number') setPiPOpacityVisual(msg.pipOpacity);
      });
      post('ready');
    }
