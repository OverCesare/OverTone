'use strict';
// Presentation state only. Commands and durable state remain owned by the C core.
let route = 'chat', detailsOpen = false, settingsSection = 'appearance', pendingChatAction = null;
let tipOwner = null, tipTimer = 0, tipHoverTimer = 0, tipCandidate = null, keyboardInput = false,
    chromeBrandAnimation = null, chromeBrandKey = '',
    chromeTransitioning = false, detailsTransition = null, detailsRevision = 0;
let downloadNoticeUntil = 0, downloadNoticeTimer = 0;
let chromeFrame = false, chromeSignature = '';
const chromeMotionBounds = new Map();
let downloadMotion = null, downloadWanted = false;
// Native menu state is event-driven and deduplicated; no synchronous WebKit queries or polling.
let appMenuQueued = false, appMenuSignature = '';
function appSearchField() {
    if (route === 'chat') return $('search');
    if (route !== 'models') return null;
    if (modelTab === 'search' && !['hf','ollama'].includes(modelSource)) return null;
    return $(modelTab === 'installed' ? 'installed-query' : modelTab === 'activity' ? 'activity-query'
        : catalogSource === 'ollama' ? 'ollama-query' : 'model-query');
}
function appMenuState() {
    const enabled = [], checked = [route];
    if (detailsOpen) checked.push('details');
    const blocked = !current || current.maintenance || current.storageFailed ||
        document.querySelector('dialog[open]') || window.guideIsActive?.();
    if (!blocked) {
        enabled.push('chat','models','knowledge','projects','tools','details','guide');
        if (!$('new').disabled) enabled.push('new-chat');
        if (!$('preferences').disabled) enabled.push('settings');
        const search = appSearchField();
        if (search && !search.disabled) enabled.push('find');
        if (current.connection.model && !$('configure').disabled) enabled.push('parameters');
        if (!$('stop').hidden && !$('stop').disabled) enabled.push('stop');
    }
    return {enabled,checked};
}
window.updateAppMenu = () => {
    if (appMenuQueued) return;
    appMenuQueued = true;
    queueMicrotask(() => {
        appMenuQueued = false;
        const state = appMenuState(), signature = JSON.stringify(state);
        if (signature === appMenuSignature) return;
        appMenuSignature = signature;
        send({command:'menuState',...state});
    });
};
function setKeyboardInput(keyboard) {
    keyboardInput = !!keyboard;
    document.documentElement.dataset.input = keyboardInput ? 'keyboard' : 'pointer';
}
window.overtoneMenu = (command, keyboard) => {
    if (!appMenuState().enabled.includes(command)) return false;
    setKeyboardInput(keyboard);
    hideTip();
    closeFloatingSurfaces();
    if (['chat','models','knowledge','projects','tools'].includes(command)) navigate(command);
    else if (command === 'new-chat') {
        navigate('chat');$('new').click();$('prompt').focus({preventScroll:true});
    } else if (command === 'find') {
        if (detailsOpen && innerWidth < 1568) setDetailsOpen(false);
        if (route === 'chat') {
            if (innerWidth < 1200) setHistoryOpen(true);
        }
        const field = appSearchField();
        if (field && !field.disabled) {field.focus({preventScroll:true});field.select();}
    } else if (command === 'guide') window.overtoneGuide();
    else $({settings:'preferences',details:'toggle-details',parameters:'configure',stop:'stop'}[command]).click();
    window.updateAppMenu();
    return true;
};
// Local developer controls use the same click paths as the UI, never arbitrary selectors or JS.
window.overtoneControl = actions => {
    if (!current?.preferences.developer || typeof actions !== 'string')
        return {ok:false,error:'control_unavailable'};
    if (actions === 'context-menu')
        return {ok:true,documents:uiDocuments.map(doc => ({reader:doc !== document,
            blocked:!doc.body.dispatchEvent(new doc.defaultView.MouseEvent('contextmenu',{bubbles:true,cancelable:true,button:2}))}))};
    if (actions === 'state') {
        const animations = uiAnimations().filter(animation =>
            Number.isFinite(animation.effect?.getComputedTiming().endTime) && animation.playState !== 'finished');
        return {ok:true,route,modelsPage:modelTab,modelSource,installedLibrary:window.installedLibrarySnapshot?.(),filterPanels:window.modelFilterSnapshot?.(),engineScan:libraryState?.engineScan,
            downloadControls:[...$('download-list').children].slice(0,8).map(row => ({
                id:row.dataset.job,state:row.querySelector('.badge')?.dataset.state,
                visible:!hiddenUIElement(row),actions:[...row.querySelectorAll('.row-actions button')]
                    .map(b => ({label:b.getAttribute('aria-label'),disabled:b.disabled}))})),
            animations:animations.slice(0,16).map(a => ({
                target:a.effect?.target?.id || a.effect?.target?.getAttribute('class') || '',
                kind:a.animationName || a.transitionProperty || 'script',
                start:a.startTime,time:a.currentTime,pending:a.pending,state:a.playState,
                end:a.effect.getComputedTiming().endTime})),
            details:detailsOpen,history:$('history-rail').classList.contains('open'),
            filters:$('chat-filters-toggle').getAttribute('aria-expanded') === 'true',
            dialog:[...document.querySelectorAll('dialog[open]')].at(-1)?.id || '',
            dialogs:[...document.querySelectorAll('dialog[open]')].slice(0,8).map(d =>
                ({id:d.id,modal:d.matches(':modal'),behind:d.classList.contains('dialog-behind'),closing:d.hasAttribute('data-closing')})),
            closingDialogs:document.querySelectorAll('dialog[data-closing]').length,
            settingsSection,
            popovers:[...document.querySelectorAll('[popover],[data-fallback-popover]')]
                .filter(surfaceIsOpen).slice(0,16).map(surface => surface.id),
            focused:document.hasFocus(),focusId:document.activeElement?.id || '',hidden:document.hidden,observers:motionSamples.size,
            composer:{chat:current.selected,length:$('prompt').value.length,pending:!!pendingSubmission,waiting,disabled:$('send').disabled,
                inputLimitBytes:current.inputLimitBytes,
                bounds:(()=>{const r=$('composer').getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height};})(),
                reviewOpen:$('improve').getAttribute('aria-expanded') === 'true',reviewPending:!!improvementRequest?.pending,
                overflow:[...$('compose-overflow').children].map(b => b.id),
                fields:['prompt','suggestion'].map(id=>{const el=$(id),r=el.getBoundingClientRect();return {id,length:el.value.length,visible:!hiddenUIElement(el),width:r.width,height:r.height,x:r.x,y:r.y,invalid:el.getAttribute('aria-invalid')==='true'};}),
                palettes:[...document.querySelectorAll('.compose-palette')].map(el=>{const r=el.getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height,radius:getComputedStyle(el).borderRadius};}),
                commands:[...document.querySelectorAll('.compose-palette button,.compose-toolbar button')].filter(b=>!hiddenUIElement(b)).map(b=>{
                    const r=b.getBoundingClientRect();return {id:b.id,x:r.x,y:r.y,width:r.width,height:r.height,disabled:b.disabled,iconWidth:b.querySelector('.icon')?.getBoundingClientRect().width,iconHeight:b.querySelector('.icon')?.getBoundingClientRect().height,radius:getComputedStyle(b).borderRadius,background:getComputedStyle(b).backgroundColor,color:getComputedStyle(b).color};})},
            runtime:{state:$('runtime-dot').dataset.state,glyph:$('runtime-dot').dataset.glyph},
            transientSurfaces:uiElements('[data-size-motion]').length,
            chromeSweeps:chromeMotionBounds.size,
            parameters:(() => {
                const d=$('model-parameters'), visible=d.open && !d.hasAttribute('data-closing');
                return {open:visible,values:visible ? Object.fromEntries([...d.querySelectorAll('input[type=range][name]')].map(i=>[i.name,i.valueAsNumber])) : {},
                    memory:visible ? [...d.querySelectorAll('.memory-track>span')].map(i=>({kind:i.className,transform:i.style.transform})) : []};
            })(),
            appearance:{theme:document.documentElement.dataset.theme,accent:document.documentElement.dataset.accent,language},
            scrolling:window.transcriptView.moving(),editing:!!editing,
            historyViewport:conversationViewport,
            branding:{name:document.title,source:brandMark.querySelector('img').getAttribute('src'),
                loaded:brandMark.querySelector('img').complete && brandMark.querySelector('img').naturalWidth === 1024,
                loading:brandMark.classList.contains('is-loading'),state:startupPending ? 'playing' : 'ready',
                filters:getComputedStyle(brandMark.querySelector('img')).filter,
                colours:[]},
            startup:(() => {
                const r=$('startup-emblem').getBoundingClientRect();
                return {visible:!startupCover.hidden,pending:startupPending,inert:startupCover.inert,
                    x:r.x,y:r.y,width:r.width,height:r.height,viewportWidth:innerWidth,viewportHeight:innerHeight};
            })(),
            configuration:window.configurationSnapshot?.(),
            connection:window.connectionSnapshot?.(),
            advice:window.adviceSnapshot?.(),
            activities:[...document.querySelectorAll('button[data-activity]')].filter(b => !hiddenUIElement(b) && b.getClientRects().length).slice(0,16).map(b => {
                const rect=b.getBoundingClientRect(), glyph=b.querySelector(':scope>.icon'), box=glyph.getBoundingClientRect();
                return {id:b.id,status:b.closest('.detected-engine')?.querySelector('small')?.textContent,phase:b.dataset.activity,busy:b.getAttribute('aria-busy'),glyph:glyph.querySelector('use').getAttribute('href'),
                    x:rect.x,y:rect.y,width:rect.width,height:rect.height,iconX:box.x,iconY:box.y,iconWidth:box.width,iconHeight:box.height};
            }),
            historyActions:[...$('history-actions').children].filter(b => !hiddenUIElement(b)).map(b => {
                const r=b.getBoundingClientRect(),c=getComputedStyle(b);return {id:b.id,width:r.width,height:r.height,
                    fill:c.backgroundColor,ink:c.color,radius:c.borderRadius,overflow:b.scrollWidth>b.clientWidth+1};
            }),
            privacy:(() => {
                const el=$('network-permissions'),box=el.closest('.network-settings'),r=box.getBoundingClientRect();
                return {visible:!hiddenUIElement(box),width:r.width,height:r.height,overflow:box.scrollWidth>box.clientWidth+1,
                    statusHidden:$('network-state').hidden,expanded:$('network-expand').getAttribute('aria-expanded')==='true'};
            })(),
            legal:(() => {
                const body=$('action-dialog-body'), close=$('close-action-dialog'), r=close.getBoundingClientRect();
                return {open:$('action-dialog').open,documents:body.querySelectorAll('.legal-license').length,
                    names:[...body.querySelectorAll('.legal-notices h3')].slice(0,32).map(n=>n.textContent),
                    closeVisible:r.width>0 && r.top>=0 && r.bottom<=innerHeight,
                    overflow:body.scrollWidth>body.clientWidth+1};
            })(),
            guidance:window.guideSnapshot?.(),
            infiniteAnimations:uiAnimations().filter(a => a.effect?.getComputedTiming().endTime === Infinity)
                .slice(0,12).map(a => ({target:a.effect.target?.id || a.effect.target?.getAttribute('class') || '',time:a.currentTime,state:a.playState})),
            checkboxes:uiElements('input[type=checkbox]:checked').filter(b => !hiddenUIElement(b) && b.getClientRects().length).slice(0,16)
                .map(b => ({id:b.id,role:b.getAttribute('role') || 'checkbox',...uiRect(b).toJSON()})),
            components:(libraryState?.engines || []).map(e => ({kind:e.kind,managed:e.managed,validation:e.validation})),
            features:{nativeScrollEnd:'onscrollend' in $('transcript'),
                scopedViewTransitions:typeof Element.prototype.startViewTransition === 'function'},
            feedback:uiElements('.app-feedback').filter(host => !host.hidden && !hiddenUIElement(host) &&
                (!host.closest('dialog') || host.closest('dialog').open)).slice(0,16)
                .map(host => ({surface:host.id || 'response',kind:host.dataset.kind,role:host.getAttribute('role')})),
            finiteAnimations:animations.length};
    }
    if (actions.startsWith('fill logo ')) {
        const phase=actions.slice(10), progress=Number(phase);
        if (phase === 'done') {finishStartup();return {ok:true};}
        if (phase !== 'master' && (!phase.trim() || !Number.isFinite(progress) || progress<0 || progress>1)) return {ok:false,error:'invalid_value'};
        startStartup();
        send({command:'startup',mode:'phase',value:phase === 'master' ? 1 : progress});
        return {ok:true};
    }
    if (actions.startsWith('fill ')) {
        const split = actions.indexOf(' ',5), fields = {url:'#download-url',checksum:'#download-checksum',
            query:'#model-query','setup-existing-query':'#setup-existing-query',purpose:'select[data-pref=purpose]','installed-query':'#installed-query','activity-query':'#activity-query','ollama-query':'#ollama-query',prompt:'#prompt',suggestion:'#suggestion',language:'select[data-pref=language]',
            server:'#endpoint','server-model':'#model',protocol:'#protocol','server-key':'#key',
            context:'#model-param-context',input:'#model-param-input',output:'#model-param-output',
            temperature:'#model-param-temperature','top-p':'#model-param-topP','memory-budget':'#model-param-memoryPercent',
            region:'select[data-pref=region]','date-format':'select[data-pref=dateFormat]',
            'chat-query':'#search','chat-title':'#new-title'}, id = fields[actions.slice(5,split)];
        if (split < 0 || !Object.hasOwn(fields,actions.slice(5,split)))
            return {ok:false,error:'unknown_field'};
        const element = document.querySelector(id), modal = [...document.querySelectorAll('dialog[open]')].at(-1),
              value = actions.slice(split+1);
        if (!element?.getClientRects().length || hiddenUIElement(element) || element.disabled ||
            modal && !modal.contains(element))
            return {ok:false,error:'control_not_visible'};
        if (element.maxLength >= 0 && value.length > element.maxLength)
            return {ok:false,error:'invalid_value'};
        if (element.options && ![...element.options].some(option => option.value === value && !option.disabled))
            return {ok:false,error:'invalid_value'};
        element.value = value;
        element.dispatchEvent(new Event('input',{bubbles:true}));
        element.dispatchEvent(new Event('change',{bubbles:true}));
        return {ok:true};
    }
    if (actions.startsWith('key ')) {
        const key = actions.slice(4);
        if (!['ArrowLeft','ArrowRight','ArrowUp','ArrowDown','Home','End','Escape','Enter',' '].includes(key))
            return {ok:false,error:'unknown_key'};
        const element = activeUIElement(), modal = [...document.querySelectorAll('dialog[open]')].at(-1);
        if (!element || hiddenUIElement(element) || element.closest('[inert]') || modal && !modal.contains(element))
            return {ok:false,error:'control_not_visible'};
        element.dispatchEvent(new element.ownerDocument.defaultView.KeyboardEvent('keydown',{key,bubbles:true,cancelable:true}));
        return {ok:true};
    }
    const focusOnly = actions.startsWith('focus ');
    if (focusOnly) actions = actions.slice(6);
    const targets = {
        'new-chat':'#new', 'first-chat':'#chats .conversation-open',
        'setup-more-existing':'#setup-existing-models > button',
        chat:'#nav-chat', models:'#nav-models', documents:'#nav-knowledge',
        projects:'#nav-projects', tools:'#nav-tools', details:'#toggle-details',
        history:'#open-history', filters:'.search-control>button[aria-controls$="-filters"]',
        dismiss:'#context-scrim.open',
        conversations:'#history-view-0', archive:'#history-view-1', trash:'#history-view-2',
        'archive-chat':'#archive-chat','restore-chat':'#restore-chat','trash-menu':'#trash-chat',
        'move-trash':'#move-trash','delete-chat':'#delete-chat','restore-all':'#restore-archive','empty-trash':'#empty-trash',
        'confirm-action':'#action-dialog-actions button:last-child','rename-chat':'#chat-title',
        'rename-save':'#rename-form .primary',
        installed:'#tab-installed,[data-model-tab=installed]', search:'#tab-search,[data-model-tab=search]',
        activity:'#tab-activity,[data-model-tab=activity]',
        'pause-download':`#download-list button[aria-label=${JSON.stringify(tr('Pausa download'))}]`,
        'resume-download':`#download-list button[aria-label=${JSON.stringify(tr('Riprendi'))}]`,
        'cancel-download':`#download-list button[aria-label=${JSON.stringify(tr('Annulla download'))}]`,
        'restart-download':`#download-list button[aria-label=${JSON.stringify(tr('Scarica di nuovo'))}]`,
        'remove-installed':'#model-list .row-actions .danger',
        'remove-reference':'#action-dialog-actions button:has([data-text-key="Rimuovi dalla lista"])',
        settings:'#preferences', parameters:'#configure', 'parameters-save':'#apply-model-parameters',
        'network-master':'#network-master','network-expand':'#network-expand',
        'network-catalogues':'[data-network="1"]','network-models':'[data-network="2"]',
        'network-engines':'[data-network="4"]','network-servers':'[data-network="8"]',
        developer:'[data-pref=developer]', 'server-enable':'#enable-remote-server',
        advice:'#show-advice','advice-refresh':'#refresh-advice','advice-acquire':'#acquire-advice',
        'advice-learning':'[data-pref="adviceLearning"]',
        'advice-notices':'[data-pref="adviceNotices"]','advice-reset':'#reset-advice',
        'advice-reset-confirm':'#confirm-reset-advice','advice-restore':'#restore-advice',
        'advice-enable':'#enable-advice-catalogues', 'advice-preferences':'#advice-preferences','advice-info':'#advice-model-info',
        'guide-next':'#next-guide','guide-previous':'#previous-guide','guide-skip':'#skip-guide',
        setup:'[data-open-setup]', 'setup-local':'#setup-local', 'setup-existing':'#setup-existing',
        'setup-skip':'#skip-setup', 'setup-back':'#setup-back', 'setup-prepare':'#setup-action',
        'setup-consent':'#setup-consent', 'setup-only-engines':'#setup-engines-only','setup-engine-consent':'#setup-engine-consent','setup-engine-confirm':'#confirm-setup-engines', 'install-engine':'#install-engine',
        'setup-engines':'#setup-engine-menu', 'setup-purposes':'#setup-purpose-menu', 'setup-size':'#setup-size-menu',
        'setup-ollama':'#setup-ollama', 'setup-llama':'#setup-llama',
        'setup-first':'#setup-models-choice-0', 'setup-second':'#setup-models-choice-1', 'setup-third':'#setup-models-choice-2',
        'setup-general':'#setup-purpose-0', 'setup-code':'#setup-purpose-1',
        'setup-server':'#setup-compatible,#setup-server', 'setup-info':'#setup-models .choice-info',
        'setup-first-existing':'#setup-existing-models .row-primary',
        'setup-existing-llama':'#setup-existing-models [data-kind="1"] .row-primary',
        'setup-pause':`#setup-jobs [data-stage=model] button[aria-label=${JSON.stringify(tr('Pausa download'))}]`,
        'setup-resume':`#setup-jobs [data-stage=model] button[aria-label=${JSON.stringify(tr('Riprendi'))}]`,
        'setup-cancel':`#setup-jobs [data-stage=model] button[aria-label=${JSON.stringify(tr('Annulla download'))}]`,
        model:'#model-menu', thinking:'#thinking-control',
        'thinking-off':'#thinking-popover button[data-thinking=off]', 'thinking-on':'#thinking-popover button[data-thinking=on]',
        link:'#acquire-url', back:'#catalog-back', preview:'#download-form button', download:'#download-confirm',
        'first-repository':'#model-results .row-primary',
        'first-ollama-repository':'#ollama-results .row-primary',
        'variant-filters':'.catalog-variants:not([hidden]) .variant-heading .icon-button',
        'more-installed':'#installed-more',
        'first-installed':'#model-list .row-primary',
        hf:'[data-catalog="hf"]', ollama:'[data-catalog="ollama"]',
        discover:'#discover-models', latest:'#latest',
        reasoning:'.reasoning-toggle', wrap:'.code-actions button:first-child,.code-viewer-actions button:first-child',
        'copy-code':'.code-actions .copy','copy-answer':'.message.assistant .turn-actions .copy',
        edit:'.message.user .turn-actions button:first-child', improve:'#improve',
        prompt:'#prompt',suggestion:'#suggestion','thinking-control':'#thinking-control',
        'discard-suggestion':'#discard-suggestion', 'accept-suggestion':'#accept-suggestion',
        'compose-more':'#compose-more',
        'cancel-edit':'.inline-editor .row button:first-child',
        send:'#send',stop:'#stop',
        expand:'.code-actions button:nth-child(2)',
        appearance:'[data-settings-section="appearance"]',
        'settings-privacy':'[data-settings-section="privacy"]', 'settings-models':'[data-settings-section="models"]', 'settings-api':'[data-settings-section="api"]',
        connection:'[data-open-connection]',
        'connection-https':'#connection-https','connection-test':'#connection-test',
        'connection-auth':'#connection-auth','connection-remember':'#connection-remember',
        'connection-save':'#save-settings','install-llama':'#install-llama',

        'engine-confirm':'#confirm-engine-install',
        'catalogues-enable':'#enable-catalogues',
        'engine-detect':'#detect-engines', 'engine-detect-llama':'#detect-engine-1', 'engine-detect-ollama':'#detect-engine-2',
        'engine-verify-llama':'#verify-engine-1', 'engine-verify-ollama':'#verify-engine-2',
        'engine-licenses-llama':'#licenses-engine-1', 'engine-licenses-ollama':'#licenses-engine-2', 'licenses':'#show-notices',
        diagnostics:'[data-settings-section="diagnostics"]'
    };
    for (const section of ['chat','storage','knowledge','tools','labs','about'])
        targets['settings-'+section] = `[data-settings-section=${section}]`;
    for (const key of ['tooltips','codeWrap','titles','summaries','improve'])
        targets['pref-'+key] = `[data-pref=${key}]`;
    for (const [group,values] of [['theme',['light','dark','system']],['accent',['blue','purple','teal','green','amber','rose']]])
        for (const value of values)
            targets[group+'-'+value] = `[data-choice-field=${group}] button[data-value=${value}]`;
    for (const gb of [0,2,4,8,12,16,24,32,48,64,96,128,256,512]) targets['setup-size-'+gb] = `#setup-sizes [data-gb="${gb}"]`;
    for (const input of document.querySelectorAll('.library-filters input[name][value]'))
        targets[input.name+'-'+input.value]='#'+CSS.escape(input.closest('.library-filters').id)+
            ' input[name="'+CSS.escape(input.name)+'"][value="'+CSS.escape(input.value)+'"]';
    for (const panel of document.querySelectorAll('.library-filters')) {
        targets[panel.id+'-reset']='#'+CSS.escape(panel.id)+'-reset';
        for (const category of panel.querySelectorAll('[data-filter-source]')) targets[category.id]='#'+CSS.escape(category.id);
    }
    for (const chip of document.querySelectorAll('.model-filter-chips button'))
        targets['clear-'+chip.dataset.filterSource+'-'+chip.dataset.filterValue]='.model-filter-chips button[data-filter-source="'+CSS.escape(chip.dataset.filterSource)+'"][data-filter-value="'+CSS.escape(chip.dataset.filterValue)+'"]';
    const list = actions.split(',');
    for (const name of list) {
        const rank = /^repository-([1-9]\d?|100)$/.exec(name);
        if (rank) targets[name] = '[data-source-page]:not([hidden]) .catalog-columns>:first-child>.library-row:nth-of-type(' + rank[1] + ') .row-primary';
    }
    if (focusOnly && (list.length !== 1 || ['close','oldest','startup','guide','setup'].includes(list[0])))
        return {ok:false,error:'unknown_action'};
    if (!list.length || list.length > 4 || list.some(name => !['close','oldest','startup','guide'].includes(name) && !Object.hasOwn(targets,name)))
        return {ok:false,error:'unknown_action'};
    for (const name of list) {
        if (name === 'guide') {window.overtoneGuide();continue;}
        if (name === 'startup') {
            if (!motionEnabled()) return {ok:false,error:'motion_unavailable'};
            replayStartup();
            continue;
        }
        if (name === 'oldest') {
            if (route !== 'chat') return {ok:false,error:'control_not_visible'};
            $('transcript').dispatchEvent(new KeyboardEvent('keydown',{key:'Home',bubbles:true}));
            continue;
        }
        if (name === 'setup') {
            // Developer inspection can reopen the one-time guide without a product button.
            window.openConfiguration();
            continue;
        }
        if (name === 'close') {
            if (window.guideAllowsClosing && !window.guideAllowsClosing()) return {ok:false,error:'guide_action_required'};
            const dialog = [...document.querySelectorAll('dialog[open]')].at(-1);
            if (!dialog) return {ok:false,error:'control_not_visible'};
            const close=dialog.querySelector('.dialog-heading > .icon-button');
            if (close && !hiddenUIElement(close) && !close.disabled) close.click();
            else closeDialog(dialog);
            continue;
        }
        const modal = [...document.querySelectorAll('dialog[open]')].at(-1);
        const named = uiElements(targets[name]).find(element => !hiddenUIElement(element) &&
            element.getClientRects().length && !element.disabled && (!modal || modal.contains(element)));
        // Named developer actions expose the normal scrollable control before
        // hit-testing it. They never bypass another surface or a modal.
        named?.scrollIntoView({block:'nearest',inline:'nearest',behavior:'instant'});
        const elements = uiElements(targets[name]).filter(element => {
            if (hiddenUIElement(element) || !element.getClientRects().length || element.disabled ||
                modal && !modal.contains(element)) return false;
            const block = element.closest('.code-block'), bounds = uiRect(block || element);
            return bounds.bottom > 0 && bounds.top < innerHeight && bounds.right > 0 && bounds.left < innerWidth;
        });
        if (!elements.length) return {ok:false,error:'control_not_visible'};
        let coveredBy = '';
        const element = elements.find(candidate => {
            const hitTarget = candidate.closest('.catalog-columns') && candidate.querySelector('.library-name') || candidate,
                  box = hitTarget.getBoundingClientRect(), doc = candidate.ownerDocument,
                  points = name === 'dismiss' ? [[.1,.1],[.9,.1],[.1,.9],[.9,.9]] : [[.5,.5]];
            return points.some(([x,y]) => {
                const px = box.left+box.width*x, py = box.top+box.height*y,
                      frame = doc.defaultView.frameElement, origin = frame?.getBoundingClientRect(),
                      outside = frame ? frame.ownerDocument.elementFromPoint(origin.left+px,origin.top+py) : null,
                      hit = frame && outside !== frame ? outside : doc.elementFromPoint(px,py);
                coveredBy = hit?.id || hit?.tagName || '';
                return candidate.contains(hit);
            });
        });
        if (!element) return {ok:false,error:'control_obscured',coveredBy:coveredBy.slice(0,64)};
        if (focusOnly) element.focus({preventScroll:true}); else element.click();
    }
    return {ok:true};
};
function surfaceClose(el) {
    if (typeof el.hidePopover === 'function') {
        if (el.matches(':popover-open'))
            el.hidePopover()
    } else if (el.id === 'tooltip')
        el.hidden = true;
    else {
        setSurfaceVisible(el, false);
        el.dispatchEvent(new Event('toggle'));
    }
    el.inert = true;
}
function surfaceOpen(el) {
    el.inert = false;
    if (typeof el.showPopover === 'function')
        el.showPopover();
    else {
        if (el.getAttribute('popover') === 'auto' || el.dataset.fallbackPopover === 'auto') {
            for (const other of document.querySelectorAll('[data-fallback-popover=auto]'))
                if (other !== el && surfaceIsOpen(other)) surfaceClose(other);
            el.dataset.fallbackPopover = 'auto';
        }
        el.removeAttribute('popover');
        if (el.id === 'tooltip') el.hidden = false;
        else {
            setSurfaceVisible(el, true);
            el.dispatchEvent(new Event('toggle'));
        }
    }
}
function closeFloatingSurfaces(preserve = []) {
    hideTip();
    for (const surface of document.querySelectorAll('[popover],[data-fallback-popover]'))
        if (!preserve.includes(surface) && surfaceIsOpen(surface)) surfaceClose(surface);
}
// Shared anchored choice menu: pointer, keyboard and the older WebKit fallback
// use the same open/close and placement primitives as existing app menus.
function bindOptionMenu(owner, menu) {
    const close = (focus = false) => {
        surfaceClose(menu);
        owner.setAttribute('aria-expanded','false');
        if (focus) owner.focus();
    };
    menu.tabIndex = -1;
    owner.setAttribute('popovertarget',menu.id);
    owner.onclick = event => {
        event.preventDefault();
        if (surfaceIsOpen(menu)) {close();return;}
        closeFloatingSurfaces();
        surfaceOpen(menu);
        positionSurface(menu,owner,owner.dataset.menuPlacement !== 'above',false,owner.dataset.menuAlign === 'end');
        owner.setAttribute('aria-expanded','true');
        if (keyboardInput) (menu.querySelector('[aria-checked=true]:not(:disabled),button:not(:disabled)') || menu).focus();
    };
    owner.addEventListener('keydown',event => {
        if (!['ArrowDown','ArrowUp'].includes(event.key)) return;
        event.preventDefault();
        if (!surfaceIsOpen(menu)) owner.click();
        const choices=[...menu.querySelectorAll('button:not(:disabled)')];
        (event.key === 'ArrowUp' ? choices.at(-1) || menu : choices[0] || menu).focus();
    });
    menu.addEventListener('toggle',() => owner.setAttribute('aria-expanded',String(surfaceIsOpen(menu))));
    document.addEventListener('pointerdown',event => {
        if (surfaceIsOpen(menu) && !owner.contains(event.target) && !menu.contains(event.target)) close();
    });
    menu.addEventListener('keydown',event => {
        const controls = [...menu.querySelectorAll('button:not(:disabled)')], index = controls.indexOf(document.activeElement);
        if (event.key === 'Escape' || event.key === 'Tab') {
            close(event.key === 'Escape');
            if (event.key === 'Escape') event.preventDefault();
        } else if (['ArrowUp','ArrowDown','Home','End'].includes(event.key)) {
            event.preventDefault();
            const next = event.key === 'Home' ? 0 : event.key === 'End' ? controls.length-1 :
                (index + (event.key === 'ArrowUp' ? -1 : 1) + controls.length) % controls.length;
            controls[next]?.focus();
        }
    });
    return close;
}
window.improveFeedback = activityFeedback($('improve'), {glyph:'wand-sparkles',busyGlyph:'loader',finish:'immediate',
    canRun:()=>!$('improve').disabled});
const closeComposerMenu = bindOptionMenu($('compose-more'),$('compose-overflow'));
$('compose-more').dataset.menuPlacement = 'above';
const composerCommands = [...$('compose-tools').children].filter(b => b.id !== 'compose-more');
for (const command of composerCommands.filter(b => b.dataset.commandLabel))
    command.append(localLabel('span',command.dataset.commandLabel,'command-label'));
let composerOverflowKey = '';
function arrangeComposerCommands() {
    const row=$('composer').querySelector('.compose-toolbar'), tools=$('compose-tools'), more=$('compose-more'),
          menu=$('compose-overflow');
    if (!row.clientWidth) return;
    const gap=parseFloat(getComputedStyle(tools).gap),
          available=row.clientWidth-row.querySelector('.compose-send-actions').offsetWidth-
              row.querySelector('.compose-context').offsetWidth-2*parseFloat(getComputedStyle(row).gap),
          capacity=Math.max(2,Math.floor((available+gap)/(36+gap))),
          candidates=composerCommands.filter(b=>b.dataset.overflow).sort((a,b)=>Number(a.dataset.overflow)-Number(b.dataset.overflow)),
          moved=new Set();
    for (const candidate of candidates) {
        if (composerCommands.length-moved.size+(moved.size ? 1 : 0) <= capacity) break;
        moved.add(candidate);
    }
    const key=composerCommands.filter(b=>moved.has(b)).map(b=>b.id).join();
    if (key === composerOverflowKey) return;
    composerOverflowKey=key;closeComposerMenu();
    for (const [parent,list,tail] of [[tools,composerCommands.filter(b=>!moved.has(b)),more],
                                   [menu,composerCommands.filter(b=>moved.has(b)),null]]) {
        let next=tail;
        for (const command of [...list].reverse()) {
            if (command.parentNode !== parent || command.nextSibling !== next) parent.insertBefore(command,next);
            if (parent === menu) command.setAttribute('role','menuitem');else command.removeAttribute('role');
            next=command;
        }
    }
    more.hidden = !moved.size;
}
const composerCommandsResize = new ResizeObserver(arrangeComposerCommands);
composerCommandsResize.observe($('composer').querySelector('.compose-toolbar'));

// Native light-dismiss/Escape must retire closing controls too. Re-enable before
// opening focus steps, and mark closed after the state changes to avoid re-entry.
document.addEventListener('beforetoggle', event => {
    if (event.target.hasAttribute?.('popover') && event.newState === 'open')
        event.target.inert = false;
}, true);
document.addEventListener('toggle', event => {
    if (event.target.hasAttribute?.('popover')) event.target.inert = event.newState === 'closed';
}, true);
if (typeof $('tooltip').showPopover !== 'function') {
    for (const el of document.querySelectorAll('[popover]'))
        el.hidden = true;
}
function positionSurface(el, owner, below = false, centered = false, alignEnd = false) {
    // Re-evaluate the authored limit on each opening, including after a resize.
    el.style.maxHeight = '';
    const r = uiRect(owner), w = el.offsetWidth, h = el.offsetHeight,
          lower = owner.closest('.shell-topbar') ?
              Math.max(r.bottom + 8, document.querySelector('.shell-topbar').getBoundingClientRect().bottom + 6) : r.bottom + 8,
          spaceBelow = Math.max(0, innerHeight - 10 - lower),
          spaceAbove = Math.max(0, r.top - 18);
    if (h > (below ? spaceBelow : spaceAbove))
        below = h <= (below ? spaceAbove : spaceBelow) ? !below : spaceBelow >= spaceAbove;
    const available = below ? spaceBelow : spaceAbove;
    if (h > available) el.style.maxHeight = available + 'px';
    const x = Math.max(10, Math.min(innerWidth - w - 10,
        centered ? r.left + (r.width - w) / 2 : alignEnd ? r.right - w : r.left));
    const y = below ? lower : r.top - el.offsetHeight - 8;
    el.style.left = x + 'px';
    el.style.top = y + 'px';
}
function hideTip() {
    clearTimeout(tipTimer);
    clearTimeout(tipHoverTimer);
    tipCandidate = null;
    const el = $('tooltip');
    surfaceClose(el);
    if (tipOwner) {
        const ids =
            (tipOwner.getAttribute('aria-describedby') || '').split(' ').filter(x => x && x !== 'tooltip');
        ids.length ? tipOwner.setAttribute('aria-describedby', ids.join(' '))
                   : tipOwner.removeAttribute('aria-describedby')
    }
    tipOwner = null;
    if (el.parentElement !== document.body)
        document.body.append(el);
}
function offersTip(owner) {
    return owner?.dataset.tip && current?.preferences.tooltips !== false &&
        (owner.hasAttribute('data-tip-overflow') || owner.getAttribute('aria-expanded') !== 'true');
}
function needsTip(owner) {
    if (owner.hasAttribute('data-tip-overflow')) {
        const label = owner.querySelector('.library-name') || owner;
        return label.clientWidth > 0 && label.scrollWidth > label.clientWidth;
    }
    // Buttons with a visible, complete caption already explain themselves.
    const words = document.createTreeWalker(owner, NodeFilter.SHOW_TEXT);
    let visibleLabel = '';
    for (let word; (word = words.nextNode());) {
        const parent = word.parentElement;
        if (!word.textContent.trim() || parent.closest('svg,[hidden],[aria-hidden=true]') ||
            getComputedStyle(parent).visibility === 'hidden')
            continue;
        const range = document.createRange();
        range.selectNodeContents(word);
        const a = range.getBoundingClientRect(), b = parent.getBoundingClientRect();
        if (Math.min(a.right, b.right) - Math.max(a.left, b.left) > 1 &&
            Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top) > 1)
            visibleLabel += word.textContent.trim() + ' ';
    }
    return visibleLabel.trim().toLocaleLowerCase() !== owner.dataset.tip.trim().toLocaleLowerCase();
}
function showTip(owner) {
    if (!offersTip(owner) || owner === tipOwner || !needsTip(owner)) return;
    const dialog = [...document.querySelectorAll('dialog[open]')].at(-1);
    if (dialog && !dialog.contains(owner))
        return;
    hideTip();
    const el = $('tooltip');
    tipOwner = owner;
    el.textContent = owner.dataset.tip;
    const host = dialog || document.body;
    host.append(el);
    surfaceOpen(el);
    positionSurface(el, owner, !!owner.closest('.shell-topbar'), true);
    if (owner.ownerDocument === document && !owner.hasAttribute('data-tip-overflow'))
        owner.setAttribute('aria-describedby',
                           ((owner.getAttribute('aria-describedby') || '') + ' tooltip').trim());
}
function queueTip(e) {
    const owner = e.target.closest('[data-tip]');
    if (!owner)
        return;
    clearTimeout(tipTimer);
    if (owner === tipCandidate || owner === tipOwner)
        return;
    hideTip();
    if (!offersTip(owner)) return;
    tipCandidate = owner;
    tipHoverTimer = setTimeout(() => {
        if (owner.isConnected && tipCandidate === owner) {
            tipCandidate = null;
            showTip(owner);
        }
    }, 500);
}
onUIEvent('pointerover', queueTip);
onUIEvent('pointermove', queueTip, {passive : true});
onUIEvent('pointerout', e => {
    if (tipCandidate && (!e.relatedTarget || !tipCandidate.contains(e.relatedTarget))) {
        clearTimeout(tipHoverTimer);
        tipCandidate = null;
    }
    if (tipOwner && (!e.relatedTarget || (!tipOwner.contains(e.relatedTarget) &&
                                        !$('tooltip').contains(e.relatedTarget))))
        tipTimer = setTimeout(hideTip, 100);
});
$('tooltip').addEventListener('pointerenter', () => clearTimeout(tipTimer));
$('tooltip').addEventListener('pointerleave', hideTip);
onUIEvent('pointerdown', e => {
    setKeyboardInput(false);
    hideTip();
    for (const surface of document.querySelectorAll('[data-fallback-popover=auto]'))
        if (surfaceIsOpen(surface) && !surface.contains(e.target) &&
            !e.target.closest('[popovertarget="' + CSS.escape(surface.id) + '"]'))
            surfaceClose(surface);
    if (!$('model-popover').contains(e.target) && !e.target.closest('#model-menu'))
        closeModelPopover();
    if (!$('thinking-popover').contains(e.target) && !e.target.closest('#thinking-control'))
        closeThinkingPopover();
    if (!$('chat-removal').contains(e.target) && !e.target.closest('#trash-chat'))
        surfaceClose($('chat-removal'));
}, true);
onUIEvent('keydown', () => {
    setKeyboardInput(true);
    hideTip();
}, true);
onUIEvent('input', hideTip, true);
onUIEvent('focusin', e => {
    if (keyboardInput)
        showTip(e.target.closest('[data-tip]'));
    else
        hideTip()
});
onUIEvent('scroll', hideTip, {capture : true, passive : true});
function animateDialog(d, opening) {
    const previous = d.dialogMotion, style = getComputedStyle(d),
          from = previous ? {opacity:style.opacity,transform:style.transform}
                          : {opacity:opening ? .4 : 1,transform:opening ? 'translateY(6px)' : 'none'};
    previous?.cancel();
    d.dialogMotion = null;
    d.inert = !opening;
    d.toggleAttribute('data-closing', !opening);
    const finish = () => {
        if (!opening) {
            // The child's close event is queued. Restore the parent first so
            // there is always a modal backdrop, including the intervening frame.
            restoreDialogParent(d);
            d.close();
        }
        d.inert = false;
        d.removeAttribute('data-closing');
    };
    if (!motionEnabled()) {
        finish();
        return;
    }
    const animation = d.animate([from, {opacity:opening ? 1 : 0,
        transform:opening ? 'none' : 'translateY(6px)'}],
        {...motionTiming(opening ? 'normal' : 'exit'), fill:'both'});
    d.dialogMotion = animation;
    measureMotion('dialog',animation);
    animation.finished.then(() => {
        if (d.dialogMotion !== animation) return;
        finish();
        animation.cancel();
        d.dialogMotion = null;
    }, () => {});
}
function closeDialog(d, dismissParents = false) {
    if (!d.open || d.hasAttribute('data-closing')) return;
    // A completed flow can leave the whole stack; Cancel still restores its parent.
    if (dismissParents) {
        let child = d;
        while (modalParents.at(-1)?.child === child) {
            const {parent} = modalParents.pop();
            parent.classList.remove('dialog-behind');
            parent.inert = false;
            parent.removeAttribute('aria-hidden');
            delete parent.dataset.restoring;
            parent.close();
            child = parent;
        }
    }
    closeFloatingSurfaces();
    animateDialog(d, false);
}
const modalParents = [], suspendedCloses = new WeakSet();
function restoreDialogParent(d) {
    const previous = modalParents.at(-1);
    if (previous?.child !== d)
        return;
    modalParents.pop();
    const {parent, focus} = previous;
    parent.classList.remove('dialog-behind');
    parent.inert = false;
    parent.removeAttribute('aria-hidden');
    suspendedCloses.add(parent);
    parent.close();
    if (parent.id === 'preferences-dialog') {
        fillPreferenceFields(current.preferences);
        syncPreferenceChoices();
        updatePreferenceModel();
    }
    parent.dataset.restoring = 'true';
    parent.showModal();
    parent.tabIndex = -1;
    if (!keyboardInput)
        parent.focus({preventScroll : true});
    else if (focus?.isConnected)
        focus.focus({preventScroll : true});
}
document.addEventListener('close', event => {
    const d = event.target;
    if (suspendedCloses.delete(d)) {
        event.stopImmediatePropagation();
        return;
    }
    const selection = getSelection();
    if (selection?.rangeCount && selection.getRangeAt(0).intersectsNode(d))
        selection.removeAllRanges();
    restoreDialogParent(d);
}, true);
function openDialog(d) {
    closeFloatingSurfaces();
    hideTip();
    closeModelPopover();
    closeThinkingPopover();
    if (d.open && d.hasAttribute('data-closing')) {
        animateDialog(d, true);
        return;
    }
    if (!d.open) {
        delete d.dataset.restoring;
        const parent = [...document.querySelectorAll('dialog[open]')].at(-1);
        d.toggleAttribute('data-nested', !!parent);
        if (parent) {
            modalParents.push({child : d, parent, focus : activeUIElement()});
            // WebKit loses the child's AX tree with two modal top-layer dialogs.
            // Keep the actual parent painted, but only the child owns modality.
            parent.dialogMotion?.cancel();
            parent.dialogMotion = null;
            parent.removeAttribute('data-closing');
            suspendedCloses.add(parent);
            parent.close();
            parent.dataset.restoring = 'true';
            parent.show();
            parent.classList.add('dialog-behind');
            parent.inert = true;
            parent.setAttribute('aria-hidden', 'true');
        }
        document.body.append(d);
        d.showModal();
        if (!keyboardInput) {
            d.tabIndex = -1;
            d.focus({preventScroll : true});
        }
        animateDialog(d, true);
    }
}
const dialogObserver = new MutationObserver(() => {hideTip();refreshFatalFeedback();window.updateAppMenu();});
for (const d of document.querySelectorAll('dialog'))
    dialogObserver.observe(d, {attributes : true, attributeFilter : [ 'open' ]});
for (const d of document.querySelectorAll('dialog')) {
    d.addEventListener('close', hideTip);
    d.addEventListener('cancel', event => {
        event.preventDefault();
        closeDialog(d);
    });
}
function actionDialog(title, body, actions = []) {
    title = tr(title);
    const d = $('action-dialog');
    setFeedback($('action-error'));
    window.closeCodeView?.();
    hideTip();
    $('action-dialog-heading').textContent = title;
    const content = $('action-dialog-body');
    content.replaceChildren(body);
    content.tabIndex = 0;
    content.setAttribute('role','region');
    content.setAttribute('aria-label',title);
    const foot = $('action-dialog-actions');
    foot.replaceChildren(...actions);
    foot.hidden = !actions.length;
    openDialog(d);
    if (keyboardInput)
        $('close-action-dialog').focus();
    return d;
}
$('close-action-dialog').onclick = () => closeDialog($('action-dialog'));
$('show-notices').onclick = () => send({command:'notices'});
window.overtoneNotices = text => {
    const body = make('div', '', 'legal-notices message-body');
    renderMarkdown(body, text);
    actionDialog('Licenze dei componenti', body);
};
window.overtoneEngineNotices = (name, payload) => {
    const body = make('div','','legal-notices message-body');
    let documents = [];
    try { documents = JSON.parse(payload); } catch {}
    for (const document of documents.sort((a,b) => a.name.localeCompare(b.name)))
        body.append(make('h3',document.name),make('pre',document.text,'legal-license'));
    if (!documents.length) body.append(localLabel('p','Licenze non disponibili.'));
    const source = name === 'Ollama' ? 'https://github.com/ollama/ollama/blob/v0.40.13/LICENSE'
                 : name === 'llama.cpp' ? 'https://github.com/ggml-org/llama.cpp/blob/b11960/LICENSE' : '';
    actionDialog(tr('Licenze dei componenti')+' · '+name,body,source ? [button('Origine',() => send({command:'openLink',url:source}))] : []);
};
window.updateAboutResources = () => {
    const r = window.lastResources;
    if (!r) return;
    const rows = [['CPU', r.cores + ' core'], [r.unified ? 'Memoria unificata' : 'Memoria', gib(r.total)],
                  ['Spazio libero', gib(r.disk)]];
    const out = $('about-resources'), signature = JSON.stringify([language,rows]);
    if (out.dataset.signature !== signature) {
        out.dataset.signature = signature;
        out.replaceChildren(...metadata(rows).children);
    }
};
function setHistoryOpen(open) {
    if ($('history-rail').classList.contains('open') !== open) closeFloatingSurfaces();
    if (open)
        setDetailsOpen(false);
    $('history-rail').classList.toggle('open', open);
    $('open-history').setAttribute('aria-expanded', String(open));
    updateContextPanels();
    if (open && innerWidth < 1200)
        $('search').focus();
}
function clearDetailsFlow(workspace, retain = null) {
    for (const animation of workspace.detailsFlow || []) animation.cancel();
    const skins = workspace.detailsSkins ||= new Map(), restore = [];
    for (const [element,clean] of skins) {
        if (retain?.has(element)) continue;
        const reset = clean();
        if (reset) restore.push(reset);
        skins.delete(element);
    }
    workspace.detailsFlow = [];
    workspace.detailsViewportClean?.();
    workspace.detailsViewportClean = null;
    if (restore.length) {
        // Resolve restored paint with transitions disabled, once for the batch.
        // Otherwise each original border/fill starts an unwanted colour tween.
        void getComputedStyle(workspace).opacity;
        for (const reset of restore) reset();
    }
}
// Structural list updates must not replace the geometry a live drawer owns.
// Keep the latest projection per list; network/engine work continues normally.
function afterDetailsMotion(owner, draw) {
    if (!detailsTransition) return false;
    owner.detailsDraw = draw;
    if (owner.detailsWait === detailsTransition) return true;
    const movement = owner.detailsWait = detailsTransition;
    const settled = () => {
        if (owner.detailsWait !== movement) return;
        owner.detailsWait = null;
        const update = owner.detailsDraw;
        owner.detailsDraw = null;
        update();
    };
    movement.finished.then(settled,settled);
    return true;
}
function reflectDetails(open) {
    const toggle=$('toggle-details');
    toggle.setAttribute('aria-expanded',String(open));
    toggle.setAttribute('aria-pressed',String(open));
    toggle.dataset.tip=tr(open ? 'Chiudi dettagli' : 'Apri dettagli');
    toggle.setAttribute('aria-label',toggle.dataset.tip);
    updateContextPanels();
}
// Keep the inspector and the catalogue on independent curves, even if the
// inspector itself changes direction before the catalogue has settled.
function inspectorCourse(originals, started, timing, catalogue) {
    const ease=layoutEasing(timing), changes=[];
    let tracks=null, accumulated=0;
    return (back,speed) => {
        const now=performance.now(), from=now-64;
        if (!tracks) tracks=originals.map(track => {
            const keys=track.effect.getKeyframes(), vector=key => {
                const m=new DOMMatrixReadOnly(key.transform);
                return [m.a,m.d,m.e,m.f];
            }, base=vector(keys.at(-1)),
            [drawer,library]=track.layoutDistances || [vector(keys[0]).map((v,i) => v-base[i]),[0,0,0,0]];
            return {node:track.effect.target,base,drawer,library};
        });
        changes.push({at:now,sign:back ? 1 : -1,ease:layoutEasing(speed),duration:speed.duration});
        while (changes[0].at+changes[0].duration<from) accumulated+=changes.shift().sign;
        const end=Math.max(started+timing.duration,catalogue.end,...changes.map(c => c.at+c.duration)),
              points=layoutSamples(from,end,time => ({
                  drawer:Math.max(0,Math.min(1,1-ease(time-started)+accumulated+changes.reduce((v,c) => v+c.sign*c.ease(time-c.at),0))),
                  library:catalogue.remaining(time)
              }));
        return tracks.map(({node,base,drawer,library}) => {
            const keys=points.map(({offset,drawer:d,library:l}) => {
                const [sx,sy,x,y]=base.map((v,i) => v+drawer[i]*d+library[i]*l);
                return {offset,transform:`matrix(${sx},0,0,${sy},${x},${y})`};
            });
            const track=node.animate(keys,{duration:end-from,fill:'both'});
            track.startTime=from;
            return track;
        });
    };
}
function setDetailsOpen(open) {
    const started = performance.now();
    const panel = $('details-panel');
    if (open === detailsOpen && (panel.redirect || panel.classList.contains('open') === open)) {
        if (open) window.renderDetails?.();
        return;
    }
    closeFloatingSurfaces();
    detailsOpen = open;
    if (open) {
        setHistoryOpen(false);
        window.renderDetails?.();
    }
    if (panel.redirect && innerWidth>=1568 && route==='models' && motionEnabled()) {
        reflectDetails(open);
        panel.redirect(open);
        return;
    }
    panel.endCombined?.();
    const revision = ++detailsRevision;
    const wasMoving = !!detailsTransition, wasPainted = panel.classList.contains('open') || wasMoving,
          style = getComputedStyle(panel), from = {transform: style.transform, opacity: style.opacity},
          docked = innerWidth >= 1568, clip = panel.previousElementSibling,
          workspace = clip.firstElementChild,
          clipFrom = getComputedStyle(clip).transform,
          contentFrom = getComputedStyle(workspace).transform,
          modelViewport = docked && route === 'models' ? document.querySelector('.model-view-surface') : null,
          modelWidth = modelViewport?.getBoundingClientRect().width || 0,
          viewport = clip.getBoundingClientRect();
    let catalogue=null;
    if (modelViewport) {
        for (const grid of modelViewport.querySelectorAll('.catalog-columns')) {
            const move=grid.catalogMotion;
            if (move?.takeFlow && grid.getClientRects().length)
                catalogue=move.takeFlow(workspace.detailsSkins ||= new Map());
            else move?.finish();
        }
    }
    const flow = docked ? [...workspace.querySelectorAll(
        '.flow-surface,.flow-anchor,.flow-surface>button,.metadata>div,.metadata dd')]
        .flatMap(el => {
            const hidden=el.closest('[hidden],[inert]');
            if ((hidden && hidden!==catalogue?.leaving) || !el.getClientRects().length) return [];
            const box = surfaceRect(el);
            return box.bottom < viewport.top - 96 || box.top > viewport.bottom + 96 ? [] : [[el,box]];
        }) : [];
    detailsTransition?.cancel();
    workspace.detailsMotion?.cancel();
    clip.detailsMotion?.cancel();
    clearDetailsFlow(workspace,new Set(flow.map(([element]) => element)));
    workspace.detailsMotion = clip.detailsMotion = null;
    detailsTransition = null;
    const releaseCatalogue = () => {
        if (catalogue?.leaving?.inert) catalogue.leaving.hidden=true;
    };
    const layout = () => panel.classList.toggle('open', open);
    panel.classList.add('animating');
    workspace.classList.toggle('details-moving',docked);
    if (route === 'chat') window.transcriptView.reflow(layout);
    else layout();
    reflectDetails(open);
    if (!motionEnabled()) {
        releaseCatalogue();
        clearDetailsFlow(workspace);
        panel.classList.remove('animating');
        workspace.classList.remove('details-moving');
        return;
    }
    if (modelViewport) {
        // The content takes its destination width once, but its scroll owner
        // must not clip moving surfaces at that final edge ahead of the drawer.
        const content = modelViewport.firstElementChild, saved = modelViewport.style.width,
              contentWidth = content.style.width, destination = modelViewport.getBoundingClientRect().width;
        content.style.width = content.getBoundingClientRect().width+'px';
        modelViewport.style.width = Math.max(modelWidth,destination)+'px';
        workspace.detailsViewportClean = () => {
            releaseCatalogue();
            modelViewport.style.width = saved;
            content.style.width = contentWidth;
        };
    }
    const hidden = {transform:'translateX(' + (panel.offsetWidth + (docked ? 0 : 16)) + 'px)', opacity:docked ? 1 : 0},
          shown = {transform:'translateX(0)',opacity:1};
    const frames = [wasPainted ? from : hidden, open ? shown : hidden];
    const timing = motionTiming(open ? 'panel' : 'panel-exit');
    if (wasMoving && docked) {
        const x = new DOMMatrixReadOnly(from.transform).m41, target = open ? 0 : panel.offsetWidth;
        timing.duration = Math.max(motionTiming('fast').duration,
            timing.duration*Math.min(1,Math.abs(target-x)/Math.max(1,panel.offsetWidth)));
    }
    const fromTime=started-64, endTime=Math.max(started+timing.duration,catalogue?.end || 0),
          combinedTiming={duration:endTime-fromTime,fill:'both'},
          ease=catalogue && layoutEasing(timing),
          points=catalogue && layoutSamples(fromTime,endTime,time => ({
              inspector:1-ease(time-started),catalogue:catalogue.remaining(time)
          }));
    const clipTo = docked ? getComputedStyle(clip).transform : '',
          contentTo = docked ? getComputedStyle(workspace).transform : '',
          moves = flow.map(([el,before]) => {
              const after = el.getBoundingClientRect();
              return [el,before,after,(Math.abs(before.width-after.width) >= .5 || catalogue?.residual.get(el)?.width) &&
                  el.matches('.flow-surface,.metadata>div') ? surfacePaint(el) : null];
          }),
          deltas = new Map(moves.map(([el,before,after]) => [el,{
              // Values belong to the row's trailing edge, even as its grid cell
              // changes width. Leading-edge compensation would move them twice.
              x:el.matches('.metadata dd') ? before.right-after.right : before.left-after.left,
              y:before.top-after.top}]));
    // A translated clipping layer and the inverse content translation share
    // one timeline. The edge moves on the compositor; layout runs only once.
    // Opposite translations cancel exactly, without scaling glyphs or corners.
    const animation = panel.animate(frames,{...timing,fill:'both',endDelay:endTime-started-timing.duration});
    const animations = [animation];
    if (docked) {
        clip.detailsMotion = clip.animate(
            [{transform:clipFrom},{transform:clipTo}], {...timing,fill:'both'});
        workspace.detailsMotion = workspace.animate(
            [{transform:contentFrom},{transform:contentTo}], {...timing,fill:'both'});
        animations.push(clip.detailsMotion,workspace.detailsMotion);
        for (const [el,before,after,paint] of moves) {
            let parent = el.parentElement;
            while (parent && parent !== workspace && !deltas.has(parent)) parent = parent.parentElement;
            const delta = deltas.get(el), ancestor = deltas.get(parent),
                  x = delta.x-(ancestor?.x || 0), y = delta.y-(ancestor?.y || 0),
                  residual=catalogue?.residual.get(el), dx=residual?.x || 0, dy=residual?.y || 0;
            if (Math.abs(x)+Math.abs(y)+Math.abs(dx)+Math.abs(dy) >= .25) {
                const keys=dx || dy ? points.map(({offset,inspector,catalogue:tail}) => ({offset,
                    transform:`translate(${x*inspector+dx*tail}px,${y*inspector+dy*tail}px)`})) :
                    [{transform:`translate(${x}px,${y}px)`},{transform:'none'}];
                const movement=el.animate(keys,dx || dy ? combinedTiming : {...timing,fill:'both'});
                if (dx || dy) {
                    movement.layoutDistances=[[0,0,x,y],[0,0,dx,dy]];
                    movement.startTime=fromTime;
                }
                workspace.detailsFlow.push(movement);
                animations.push(movement);
            }
            const widthPath=residual?.width ? {start:fromTime,timing:combinedTiming,
                inspector:before.width-after.width,catalogue:residual.width,
                points:points.map(({offset,inspector,catalogue:tail}) => ({
                    offset,length:(before.width-after.width)*inspector+residual.width*tail
                }))} : null;
            const size = resizeSurface(el,before,after,paint,timing,'X',widthPath);
            workspace.detailsFlow.push(...size.effects);
            if (size.effects.length) workspace.detailsSkins.set(el,size.clean);
            animations.push(...size.effects);
        }
    }
    for (const effect of animations) effect.finished.catch(() => {});
    // In a hand-off both gestures share the existing document timeline. Ordinary
    // opens and closes start together at the next rendering opportunity.
    if (catalogue) {
        for (const effect of animations) if (effect.startTime===null) effect.startTime=started;
    }
    let active=animations;
    const finish=() => {
        if (revision !== detailsRevision) return;
        panel.redirect=panel.endCombined=null;
        panel.classList.toggle('open',detailsOpen);
        if (active!==animations) for (const track of active) track.cancel();
        animation.cancel();
        workspace.detailsMotion?.cancel();
        clip.detailsMotion?.cancel();
        workspace.detailsMotion = clip.detailsMotion = null;
        workspace.classList.remove('details-moving');
        clearDetailsFlow(workspace);
        panel.classList.remove('animating');
        detailsTransition = null;
        scheduleChromeTargets();
        if (detailsOpen && keyboardInput)
            $('details-tabs').querySelector('[aria-selected=true]')?.focus();
    };
    const watch=(group,since) => {
        detailsTransition=group[0];
        measureMotion('details',group[0],performance.now()-since);
        group[0].finished.then(() => {if (active===group) finish();},() => {});
    };
    if (catalogue) {
        const advance=inspectorCourse(animations,started,timing,catalogue);
        panel.endCombined=finish;
        panel.redirect=target => {
            const now=performance.now(), previous=active;
            active=advance(target!==open,motionTiming(target ? 'panel' : 'panel-exit'));
            for (const track of previous) track.cancel();
            watch(active,now);
        };
    }
    watch(animations,started);
}
function updateContextPanels() {
    const history = $('history-rail'), details = $('details-panel');
    const historyFloating = innerWidth < 1200;
    const historyVisible = historyFloating && history.classList.contains('open');
    window.updateAppMenu();
    history.classList.toggle('floating-panel', historyFloating);
    const detailsFloating = innerWidth < 1568, detailsVisible = detailsFloating && detailsOpen;
    details.inert = !detailsOpen;
    history.inert = historyFloating && !historyVisible;
    document.querySelector('.chat-panel').inert = historyVisible || detailsVisible;
    $('model-workspace').inert = detailsVisible;
    $('prepared-workspace').inert = detailsVisible;
    $('context-scrim')?.classList.toggle('open', historyVisible || detailsVisible);
    highlightVisibleCode();
    scheduleChromeTargets();
}
function closeContextPanel() {
    if ($('history-rail').classList.contains('open')) {
        setHistoryOpen(false);
        $('open-history').focus({preventScroll:true});
    } else if (detailsOpen) {
        setDetailsOpen(false);
        $('toggle-details').focus({preventScroll:true});
    }
    hideTip();
}
window.setDetailsOpen = setDetailsOpen;
$('toggle-details').onclick = () => setDetailsOpen(!detailsOpen);
const contextScrim = make('div', '', 'context-scrim');
contextScrim.id = 'context-scrim';
contextScrim.onclick = closeContextPanel;
document.body.append(contextScrim);
$('open-history').onclick = () => setHistoryOpen(!$('history-rail').classList.contains('open'));
function setSurfaceVisible(element, visible, {collapse = false, large = false} = {}) {
    if (collapse && element.classList.contains('reasoning-body')) return revealReadingBody(element,visible);
    if (collapse && element.id === 'suggestion-panel') return revealSuggestion(element,visible);
    const started = performance.now();
    // Repeated projections must not restart an unfinished transition to the same state.
    if (element.surfaceVisible === visible && element.surfaceAnimation)
        return;
    element.surfaceVisible = visible;
    const hidden = element.hidden, previous = element.surfaceAnimation;
    if (!previous && hidden === !visible) {
        element.inert = !visible;
        return;
    }
    const opacity = hidden ? 0 : Number(getComputedStyle(element).opacity);
    previous?.cancel();
    element.surfaceAnimation = null;
    element.removeAttribute('data-surface-motion');
    element.inert = !visible;
    element.hidden = false;
    if (!motionEnabled() || hiddenUIElement(element.parentElement) || !element.getClientRects().length) {
        element.hidden = !visible;
        return;
    }
    const speed = large ? (visible ? 'panel' : 'panel-exit') : (visible ? 'normal' : 'exit');
    const animation = element.animate([{opacity},{opacity:visible ? 1 : 0}], {...motionTiming(speed), fill:'both'});
    measureMotion(element.id === 'chat-filters' ? 'filters' : element.classList.contains('reasoning-body') ? 'reasoning' : element.classList.contains('catalog-variants') ? 'variants' : 'surface',animation,performance.now() - started);
    element.surfaceAnimation = animation;
    element.setAttribute('data-surface-motion','');
    animation.finished.then(() => {
        if (element.surfaceAnimation !== animation)
            return;
        element.hidden = !visible;
        animation.cancel();
        element.surfaceAnimation = null;
        element.removeAttribute('data-surface-motion');
        scheduleChromeTargets();
    }, () => {});
}
function enterSurface(element, setupMs = 0) {
    if (!element || !motionEnabled() || hiddenUIElement(element))
        return;
    element.entranceAnimation?.cancel();
    const animation = element.animate(
        [ {opacity: .4, transform : 'translateY(4px)'}, {opacity: 1, transform : 'translateY(0)'} ],
        motionTiming());
    measureMotion('route',animation,setupMs);
    element.entranceAnimation = animation;
    animation.finished.then(() => {
        if (element.entranceAnimation === animation) element.entranceAnimation = null;
        scheduleChromeTargets();
    }, () => {});
}
let reportedChatVisibility = null;
function syncChatVisibility() {
    const visible = route === 'chat' && !document.hidden;
    if (visible !== reportedChatVisibility) {
        reportedChatVisibility = visible;
        send({command:'chatVisibility',visible});
    }
}
function syncWindowMotion() {
    syncChatVisibility();
    window.syncReaderVisibility?.();
    const active = !document.hidden;
    if (document.documentElement.dataset.active !== String(active))
        document.documentElement.dataset.active = String(active);
    if (active) {
        window.flushChatProgress?.();
        highlightVisibleCode();
    }
    queueMicrotask(scheduleChromeTargets);
    updateDiscoveryFeedback(!!libraryState?.discovering);
    if (!motionEnabled()) {
        if (document.hidden) closeFloatingSurfaces(window.activeGuideSurfaces?.() || []);
        finishSuggestionMotion();
        for (const animation of uiAnimations())
            if (scriptMotion(animation) &&
                Number.isFinite(animation.effect?.getComputedTiming().endTime) &&
                !(chromeTransitioning && animation === chromeBrandAnimation))
                animation.finish();
    }
}
window.addEventListener('focus', syncWindowMotion);
window.addEventListener('blur', syncWindowMotion);
document.addEventListener('visibilitychange', syncWindowMotion);
motionPreference.addEventListener('change',syncWindowMotion);
syncWindowMotion();
// Measure once per selection. The moving surface is independent of content layout;
// WebKit samples the animations at the display refresh rate, including interrupted moves.
function selectionBounds(group) {
    const surface = group.querySelector('.selection-surface');
    if (!surface)
        return null;
    const left = surface.firstElementChild.getBoundingClientRect(),
          right = surface.lastElementChild.getBoundingClientRect();
    const width = right.right-left.left;
    return width > 0 && left.height > 0 ?
        {left:left.left,top:left.top,width,height:left.height} : null;
}
function selectionMotion(group, selected, previous, animate = false) {
    if (!selected || !group.getClientRects().length)
        return;
    let surface = group.querySelector('.selection-surface');
    if (!surface) {
        surface = make('span', '', 'selection-surface');
        surface.setAttribute('aria-hidden', 'true');
        surface.append(make('i', '', 'selection-start'), make('i', '', 'selection-fill'),
                       make('i', '', 'selection-end'));
        group.prepend(surface);
    }
    const bounds = group.getBoundingClientRect(),
          target = selected.getBoundingClientRect();
    const animated = animate && previous?.width && motionEnabled();
    surface.style.setProperty('--selection-radius', getComputedStyle(selected).borderTopLeftRadius);
    const position = {left: target.left - bounds.left - group.clientLeft + group.scrollLeft + 'px',
                      top: target.top - bounds.top - group.clientTop + group.scrollTop + 'px',
                      width: target.width + 'px', height: target.height + 'px'};
    Object.assign(surface.style, position);
    if (animated) {
        const timing = motionTiming();
        // Three slices preserve the corner radius while the middle scales on the compositor.
        const inset = surface.children[1].offsetLeft;
        return [
            [surface, `translate(${previous.left - target.left}px,${previous.top - target.top}px)`],
            ...surfaceSlices(surface.lastElementChild,surface.children[1],previous.width,target.width,inset)
        ].map(([element, from]) => [element,[{transform:from},{transform:'none'}],timing]);
    }
    return [];
}
function selectionChange(group, change, selector) {
    const buttons = [...group.querySelectorAll('button')];
    const before = group.pendingSelection?.before || buttons.map(b => b.getBoundingClientRect());
    // A group first shown after startup may not have its moving surface yet.
    // Capture the old selected button before changing state, never the destination.
    const previous = group.pendingSelection?.previous || selectionBounds(group) ||
        group.querySelector(selector)?.getBoundingClientRect();
    change();
    for (const animation of group.selectionAnimations || []) animation.cancel();
    group.selectionAnimations = [];
    const reduced = !motionEnabled();
    const after = buttons.map(b => b.getBoundingClientRect());
    const selected = group.querySelector(selector), index = buttons.indexOf(selected),
          plans = selectionMotion(group, selected, previous, true) || [],
          label = selected?.querySelector('span');
    if (!reduced && label?.getClientRects().length) {
        const frames = [{opacity:.2},{opacity:1}];
        if (index >= 0 && after[index].width-before[index].width > .5) {
            // Reveal a growing label only as its neighbours make room for it.
            frames[0].clipPath = 'inset(0 100% 0 0)';
            frames[1].clipPath = 'inset(0)';
        }
        plans.push([label,frames,motionTiming()]);
    }
    buttons.forEach((button, i) => {
        if (!reduced && before[i].width && after[i].width) {
            const delta = before[i].left - after[i].left;
            if (Math.abs(delta) > .1) {
                plans.push([button,[{transform:`translateX(${delta}px)`},{transform:'none'}],motionTiming()]);
            }
        }
    });
    const pending = group.pendingSelection = {before,previous};
    queueMicrotask(() => {
        if (group.pendingSelection !== pending) return;
        group.pendingSelection = null;
        if (!motionEnabled()) { selectionMotion(group,group.querySelector(selector)); return; }
        const animations = group.selectionAnimations = plans.map(([el,frames,timing]) => el.animate(frames,timing));
        if (animations.length) measureMotion('selection',animations[0]);
        Promise.all(animations.map(a => a.finished)).then(() => {
            if (group.selectionAnimations !== animations) return;
            group.selectionAnimations = [];
            selectionMotion(group,group.querySelector(selector));
            scheduleChromeTargets();
        }, () => {});
    });
}
window.selectionChange = selectionChange;
const selectionObserver = new ResizeObserver(entries => {
    for (const {target} of entries)
        if (!target.getAnimations({subtree: true}).some(a => a.pending || a.playState === 'running'))
            selectionMotion(target, target.querySelector('[aria-current=page],[aria-selected=true]'));
});
for (const group of [document.querySelector('.top-navigation'), $('history-filter'), $('catalog-scopes')])
    selectionObserver.observe(group);
function navigate(next) {
    if (!['chat', 'models', 'knowledge', 'projects', 'tools'].includes(next))
        return;
    const started = performance.now(), chatPane = document.querySelector('.chat-panel');
    closeFloatingSurfaces();
    closeModelPopover();
    closeThinkingPopover();
    if (route === 'chat') {
        if (next !== route) {
            finishSuggestionMotion();
            window.transcriptView.suspend();
        }
        flushDraft();
    }
    const changed = route !== next;
    if (changed && route === 'chat') historyFilterMotion?.clean();
    route = next;
    syncChatVisibility();
    // Measure the navigation before exposing a large document. Its two geometry
    // passes must not repeatedly lay out a just-revealed conversation/catalogue.
    selectionChange(document.querySelector('.top-navigation'), () => {
        for (const b of document.querySelectorAll('.top-navigation [data-route]'))
            b.setAttribute('aria-current', b.dataset.route === next ? 'page' : 'false');
    }, '[aria-current=page]');
    $('chat-workspace').hidden = next !== 'chat' && innerWidth < 1568;
    $('chat-workspace').inert = next !== 'chat';
    $('chat-workspace').classList.toggle('inactive-view',next !== 'chat');
    window.syncReaderVisibility?.();
    if (next !== 'chat')
        chatPane.entranceAnimation?.cancel();
    $('model-workspace').hidden = next !== 'models';
    $('prepared-workspace').hidden = [ 'chat', 'models' ].includes(next);
    updateDiscoveryFeedback(!!libraryState?.discovering);
    const label = {
        chat : 'Chat',
        models : 'Modelli',
        knowledge : 'Documenti',
        projects : 'Progetti',
        tools : 'Strumenti'
    }[next];
    $('area-label').textContent = tr(label);
    $('details-subtitle').textContent = tr(label);
    if (!['chat', 'models'].includes(next))
        renderPrepared(next);
    if (changed && !['chat','models'].includes(next))
        document.querySelector('.prepared-content').scrollTop = 0;

    if (next === 'models')
        updateLibraryDetails();
    refreshFatalFeedback();
    window.renderDetails?.();
    setHistoryOpen(false);
    if (changed && innerWidth < 1568)
        setDetailsOpen(false);
    if (changed && next === 'chat') {
        window.flushChatProgress?.();
        window.transcriptView.resumeGeneration();
        window.transcriptView.settle();
        highlightVisibleCode();
    }
    if (changed)
        queueMicrotask(() => {
            // Flush pending visible text/highlighting before starting the arrival,
            // rather than consuming its first frames with that preparation.
            if (route === next)
                // Keep the floating history outside the conversation's animated
                // stacking context, above its dismissal scrim even during entry.
                enterSurface(next === 'chat' ? chatPane :
                             $(next === 'models' ? 'model-workspace' : 'prepared-workspace'),
                             performance.now() - started);
        });
    window.updateAppMenu();
    recordMotionWork('navigation',started);
}
window.navigate = navigate;
for (const b of document.querySelectorAll('[data-route]'))
    b.onclick = () => navigate(b.dataset.route);
const tabs = make('div', '', 'mobile-model-tabs segmented-control');
selectionObserver.observe(tabs);
tabs.setAttribute('role', 'tablist');
localAccessible(tabs, 'Modelli');
for (const [id, label, glyph] of [[ 'installed', 'Installati', 'box' ], [ 'search', 'Trova', 'search' ],
                                  [ 'activity', 'Attività', 'download' ]]) {
    const b = button(label, () => modelPage(id));
    b.dataset.modelTab = id;
    b.setAttribute('role', 'tab');
    b.setAttribute('aria-selected', String(id === modelTab));
    b.replaceChildren(icon(glyph), localLabel('span', label));
    if (id !== 'search') {
        const count = make('small', '', 'view-count');
        count.id = 'compact-' + id + '-count';
        count.hidden = true;
        b.append(count);
    }
    tabs.append(b)
}
$('model-view-title').before(tabs);
let composerText = '', composerBytes = 0;
const composerMeasures = new WeakMap();
const inputEncoder = new TextEncoder();
// One sizing rule for the draft and its review; no nested browser resize handle.
window.fitComposerText = input => {
    if (!input || input.hidden || input.closest('[hidden]')) return false;
    const width = input.clientWidth, previous = composerMeasures.get(input);
    if (previous?.text === input.value && previous.width === width && previous.viewport === innerHeight) return false;
    const style = getComputedStyle(input), base = parseFloat(style.minHeight), maximum = parseFloat(style.maxHeight),
          before = input.style.height;
    const padding = parseFloat(style.paddingTop) + parseFloat(style.paddingBottom), line = parseFloat(style.lineHeight),
          limit = Math.max(base, Math.floor((maximum - padding) / line) * line + padding);
    input.style.height = 'auto';
    // Stop between lines rather than cutting through the last visible line.
    input.style.height = (input.value ? Math.min(limit, Math.max(base, input.scrollHeight)) : base) + 'px';
    composerMeasures.set(input,{text:input.value,width,viewport:innerHeight});
    return input.style.height !== before;
};
window.updateComposer = () => {
    const input = $('prompt'), activeInput = $('improve').getAttribute('aria-expanded') === 'true' ? $('suggestion') : input;
    if (window.fitComposerText(activeInput)) window.transcriptView?.settle();
    if (composerText !== activeInput.value) {
        composerText = activeInput.value;
        composerBytes = inputEncoder.encode(composerText).length;
    }
    const overLimit = !!current && composerBytes > current.inputLimitBytes;
    activeInput.setAttribute('aria-invalid', String(overLimit));
    const limitError = $('input-limit-error');
    setFeedback(limitError,
        overLimit ? 'Il messaggio supera il limite di input. Riduci il testo o aumenta il contesto.' : '', 'warning');
    if (overLimit) {
        activeInput.setAttribute('aria-describedby', 'input-limit-error');
        $('send').disabled = true;
    } else
        activeInput.removeAttribute('aria-describedby');
    const options = thinkingOptions(), mode = effectiveThinking(options);
    $('thinking-control').dataset.active = String(!['auto', 'off', 'none'].includes(mode));
    $('thinking-control').disabled =
        !current?.ready || options.length === 1 || !!(waiting || current.maintenance || current.storageFailed);
    $('thinking-control').dataset.tip = tr(!current?.ready                    ? 'Seleziona un modello'
                                           : current.thinkingKnown && options.length === 1 ? 'Ragionamento non supportato.'
                                           : options.length === 1 ? 'Supporto Thinking non verificato.'
                                                                  : 'Ragionamento') +
                                        (options.length > 1 ? ': ' + thinkingLabel(mode) : '');
};
new ResizeObserver(() => window.updateComposer()).observe($('composer').querySelector('.composer-writing'));
function clearDownloadMotion() {
    const motion = downloadMotion;
    if (!motion) return;
    downloadMotion = null;
    for (const effect of motion.effects) effect.cancel();
    const restore = motion.clean?.();
    if (restore) { void getComputedStyle(document.querySelector('.model-control')).opacity; restore(); }
    chromeMotionBounds.delete('download');
    scheduleChromeTargets();
}
function downloadLayout(visible) {
    const indicator = $('download-indicator'), field = document.querySelector('.model-control'),
          nodes = [field,$('runtime-dot'),$('model-menu')], before = nodes.map(surfaceRect),
          opacity = Number(getComputedStyle(indicator).opacity), wasVisible = indicator.classList.contains('visible'),
          paint = surfacePaint(field);
    clearDownloadMotion();
    indicator.classList.toggle('visible',visible);
    const after = nodes.map(node => node.getBoundingClientRect()), parent = indicator.parentElement.getBoundingClientRect(),
          preferences = $('preferences').getBoundingClientRect(),
          gap = parseFloat(getComputedStyle(indicator.parentElement).columnGap) || 0;
    indicator.style.setProperty('--download-right',parent.right-preferences.left+gap+'px');
    if (!motionEnabled()) return;
    const timing = motionTiming(), effects = [], delta = before[0].left-after[0].left;
    nodes.forEach((node,index) => {
        const x = before[index].left-after[index].left-(index ? delta : 0);
        if (Math.abs(x) >= .25)
            effects.push(node.animate([{transform:`translateX(${x}px)`},{transform:'none'}],timing));
    });
    const size = resizeSurface(field,before[0],after[0],paint,timing);
    effects.push(...size.effects);
    if (visible && (!wasVisible || opacity < .999)) effects.push(indicator.animate(wasVisible ? [{opacity},{opacity:1}] :
        [{opacity:0},{opacity:0,offset:.8},{opacity:1}],timing));
    if (!effects.length) return;
    const left = Math.min(before[0].left,after[0].left), right = Math.max(before[0].right,after[0].right);
    // AppKit's drag exclusion covers the swept region, while WebKit keeps exact
    // hit testing. No per-frame geometry polling is needed for moving controls.
    chromeMotionBounds.set('download',[left,after[0].top,right-left,after[0].height]);
    const motion = downloadMotion = {effects,clean:size.clean};
    measureMotion('indicator',effects[0]);
    Promise.all(effects.map(effect => effect.finished)).then(() => {
        if (downloadMotion === motion) clearDownloadMotion();
    },() => {});
    scheduleChromeTargets();
}
function setDownloadVisible(visible) {
    if (visible === downloadWanted) return;
    downloadWanted = visible;
    const indicator = $('download-indicator');
    if (visible || !motionEnabled() || !indicator.classList.contains('visible')) {
        downloadLayout(visible);
        return;
    }
    const opacity = Number(getComputedStyle(indicator).opacity);
    clearDownloadMotion();
    const fade = indicator.animate([{opacity},{opacity:0}],{...motionTiming('exit'),fill:'both'}),
          motion = downloadMotion = {effects:[fade]};
    measureMotion('indicator',fade);
    fade.finished.then(() => {
        if (downloadMotion === motion && !downloadWanted) downloadLayout(false);
    },() => {});
}
window.updateShell = () => {
    const selected = activeLibraryModel(),
          name = modelLabel(selected) || connectionLabel() || tr('Modelli'),
          controlLabel = current?.connection.model ? 'Parametri del modello' : 'Modelli';
    $('active-model').textContent = name;
    document.querySelector('.model-control').classList.toggle('no-model',!selected && !current?.connection.model);
    $('model-menu').disabled = !current || !!(current.active || current.improving);
    $('configure').setAttribute('aria-label', tr(controlLabel));
    $('configure').dataset.tip = tr(controlLabel);
    const {state,glyph,label} = runtimeStatus(), status = $('runtime-dot');
    if (status.dataset.state !== state || status.dataset.glyph !== glyph) {
        status.dataset.state = state;
        status.dataset.glyph = glyph;
        status.replaceChildren(icon(glyph));
    }
    status.setAttribute('aria-label', label);
    status.dataset.tip = label;
    if (tipOwner === status) {
        $('tooltip').textContent = label;
        positionSurface($('tooltip'), status, true, true);
    }
    const job = libraryState?.jobs.find(j => ['preparing','downloading','verifying','registering'].includes(j.state)) ||
                libraryState?.jobs.find(j => ['queued','paused'].includes(j.state)),
          indicator = $('download-indicator');
    const visible = !!job || performance.now() < downloadNoticeUntil;
    $('download-arc').style.strokeDashoffset =
        String(106.815 * (1 - (jobFraction(job) ?? 0)));
    setDownloadVisible(visible);
    indicator.inert = !visible;
    indicator.setAttribute('aria-hidden', String(!visible));
    indicator.classList.toggle('active', !!job);
    indicator.classList.toggle('moving', [ 'preparing', 'downloading' ].includes(job?.state));
    indicator.dataset.tip =
        job ? modelLabel(job) + ' · ' +
                  jobLabel(job) +
                  (jobFraction(job) !== null ? ' · ' + Math.floor(jobFraction(job) * 100) + '%' : '')
            : tr('Attività download');
    updateComposer();
    window.updateChromeAppearance?.();
    window.updateCatalogPolicy?.();
    window.renderDetails?.();
    window.flushCodePreference?.();
    window.refreshModelParameters?.();
    if ($('preferences-dialog').open)
        updatePreferenceModel();
};
$('download-indicator').onclick = () => {
    navigate('models');
    modelPage('activity')
};
function closeModelPopover() {
    surfaceClose($('model-popover'));
    $('model-menu').setAttribute('aria-expanded', 'false')
}
$('model-menu').onkeydown = e => {
    if ([ 'ArrowDown', 'ArrowUp' ].includes(e.key)) {
        e.preventDefault();
        if (!surfaceIsOpen($('model-popover')))
            $('model-menu').click();
    }
};
for (const id of ['model-popover', 'thinking-popover'])
    $(id).onkeydown = e => {
        if (e.key === 'Tab' && id === 'thinking-popover') {
            closeThinkingPopover();
            $('thinking-control').focus();
            hideTip();
            return;
        }
        if (!['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(e.key))
            return;
        const items = [...$(id).querySelectorAll('button:not(:disabled)') ];
        if (!items.length)
            return;
        e.preventDefault();
        const i = items.indexOf(document.activeElement);
        const n = e.key === 'Home'  ? 0
                  : e.key === 'End' ? items.length - 1
                                    : (i + (e.key === 'ArrowDown' ? 1 : -1) + items.length) % items.length;
        items[n].focus();
    };
$('model-menu').setAttribute('popovertarget', 'model-popover');
$('model-menu').onclick = event => {
    event.preventDefault();
    closeThinkingPopover();
    const p = $('model-popover');
    if (surfaceIsOpen(p)) {
        closeModelPopover();
        return
    }
    hideTip();
    p.replaceChildren();
    for (const m of libraryState?.models || []) {
        const blocked = m.kind === 1 && (!m.available || m.metadata?.fileChanged);
        const b = button(modelLabel(m), () => {
            closeModelPopover();
            if (blocked) {
                navigate('models');
                modelPage('installed');
                selectedLibraryModel = m.id;
                renderInstalled();
                updateLibraryDetails(true);
                return;
            }
            libraryCommand('select', {id : m.id})
        }, true), text = make('span', modelLabel(m));
        text.append(make('small', blocked ? tr(m.available ? 'Da verificare' : 'Non disponibile') :
            (m.kind === 1 ? 'llama.cpp · GGUF' : 'Ollama') + ' · ' + sizes(m.bytes)));
        b.replaceChildren(icon(m.kind === 1 ? 'cpu' : 'box'), text);
        b.disabled = !!(current?.active || current?.improving);
        if (m.id === libraryState.selected) {
            b.className = 'selected';
            b.append(icon('check'))
        }
        p.append(b)
    }
    if (!libraryState?.models.length)
        p.append(emptyModel(tr('Nessun modello locale trovato.')));
    if (!libraryState?.models.length) {
        const browse = button('Trova un modello', () => {
            closeModelPopover();
            navigate('models');
            modelPage('search')
        });
        browse.prepend(icon('search'));
        p.append(browse);
    }
    surfaceOpen(p);
    positionSurface(p, $('model-menu'), true);
    $('model-menu').setAttribute('aria-expanded', 'true');
    hideTip();
    if (keyboardInput)
        (p.querySelector('.selected') || p.querySelector('button'))?.focus();
};
window.openConnection = () => {
    hideTip();
    prepareConnection();
    setFeedback($('settings-error'));
    openDialog($('settings'))
};
for (const b of document.querySelectorAll('[data-open-connection]'))
    b.onclick = () => {
        hideTip();
        openConnection()
    };
for (const b of document.querySelectorAll('[data-open-details]'))
    b.onclick = () => {
        closeDialog($('preferences-dialog'));
        setDetailsOpen(true)
    };
function preferenceSection(id) {
    const changed = settingsSection !== id;
    settingsSection = id;
    if (id === 'about') window.updateAboutResources();
    for (const b of document.querySelectorAll('[data-settings-section]'))
        b.setAttribute('aria-current', String(b.dataset.settingsSection === id));
    for (const p of document.querySelectorAll('[data-settings-page]'))
        p.hidden = p.dataset.settingsPage !== id;
    if (changed) {
        document.querySelector('.preferences-content').scrollTop = 0;
        enterSurface(document.querySelector('[data-settings-page="' + id + '"]'));
    }
    const b = document.querySelector('[data-settings-section="' + id + '"]');
    if (innerWidth <= 430)
        b?.scrollIntoView({block : 'nearest', inline : 'nearest'})
}
for (const b of document.querySelectorAll('[data-settings-section]'))
    b.onclick = () => {
        preferenceSection(b.dataset.settingsSection);
        b.focus()
    };
const originalPreferences = $('preferences').onclick;
$('preferences').onclick = () => {
    hideTip();
    if (!current)
        return;
    originalPreferences();
    preferenceSection(settingsSection);
    syncPreferenceChoices();
    updatePreferenceModel();

};
// The restored settings surface saves each committed change, without closing the dialog.
function updatePreferenceModel() {
    const diagnostic = $('developer-diagnostics'), wasHidden = diagnostic.hidden;
    diagnostic.hidden = !current?.preferences.developer;
    if (wasHidden && !diagnostic.hidden)
        refreshDiagnostics();
    const thinking = document.querySelector('[data-pref="thinkingMode"]'),
          group = document.querySelector('[data-choice-field="thinkingMode"]'),
          values = ['auto', 'on', 'off'], visibleValues = values,
          thinkingSignature = JSON.stringify([language, values]);
    if (thinking.dataset.options !== thinkingSignature) {
        thinking.dataset.options = thinkingSignature;
        thinking.replaceChildren(...visibleValues.map(value => {
            const option = new Option(thinkingLabel(value), value);
            option.disabled = !values.includes(value);
            return option;
        }));
        group.replaceChildren(...visibleValues.map(value => {
            const option = button(thinkingLabel(value), () => {});
            option.type = 'button';
            option.dataset.value = value;
            option.setAttribute('role', 'radio');
            return option;
        }));
    }
    thinking.value = values.includes(current.preferences.thinkingMode) ? current.preferences.thinkingMode : 'auto';
    thinking.disabled = false;
    for (const option of group.querySelectorAll('button'))
        option.disabled = thinking.disabled || !values.includes(option.dataset.value);
    syncPreferenceChoices();
    const select = $('preference-model');
    const signature = JSON.stringify(
        [ language, current?.connection.model, libraryState?.models.map(m => [m.id, m.name]) ]);
    if (select.dataset.signature !== signature) {
        select.dataset.signature = signature;
        select.replaceChildren(new Option(tr('Ultimo modello usato'), '0'));
        for (const m of libraryState?.models || []) {
            const option = make('option', modelLabel(m));
            option.value = m.id;
            select.append(option);
        }
        select.append(new Option(tr('Nessuno'), '-1'));
    }
    const selectedId = String(current.preferences.startupModel || 0);
    if (select.value !== selectedId)
        select.value = selectedId;
    select.disabled = !current || !!current.maintenance;

}
let diagnosticPending = false;
function refreshDiagnostics() {
    if (diagnosticPending || !current?.preferences.developer)
        return;
    diagnosticPending = true;
    $('refresh-diagnostics').disabled = true;
    $('diagnostic-status').replaceChildren(localLabel('span', 'Lettura dello stato'));
    send({command : 'diagnostics'});
}
window.overtoneDiagnostic = payload => {
    diagnosticPending = false;
    $('refresh-diagnostics').disabled = false;
    if (!current?.preferences.developer)
        return;
    try {
        $('diagnostic-output').textContent = JSON.stringify(JSON.parse(payload), null, 2);
        $('diagnostic-output').hidden = false;
        const stamp = make('time');
        setTimestamp(stamp, Date.now() / 1000, true);
        $('diagnostic-status').replaceChildren(localLabel('span', 'Stato acquisito'), ' · ', stamp);
    } catch {
        $('diagnostic-status').replaceChildren(localLabel('span', 'Stato non disponibile'));
    }
};
$('refresh-diagnostics').onclick = refreshDiagnostics;
$('preference-model').onchange = () => send({command : 'preferences', value : {...current.preferences, startupModel : Number($('preference-model').value)}});

function syncPreferenceChoices() {
    for (const group of document.querySelectorAll('[data-choice-field]')) {
        const value = document.querySelector('[data-pref="' + group.dataset.choiceField + '"]').value;
        for (const b of group.querySelectorAll('[data-value]')) {
            b.setAttribute('aria-checked', String(b.dataset.value === value));
            b.tabIndex = b.dataset.value === value ? 0 : -1;
        }
    }
}
window.syncPreferenceChoices = syncPreferenceChoices;
for (const label of document.querySelectorAll('.settings-fields > label')) {
    const texts = [...label.childNodes ].filter(n => n.nodeType === Node.TEXT_NODE && n.textContent.trim());
    if (texts.length) {
        const name = make('span');
        texts.forEach(n => {
            n.textContent = n.textContent.trim();
            name.append(n);
        });
        label.prepend(name);
    }
    label.classList.add('setting-row');
    const check = label.querySelector('input[type=checkbox]');
    if (check)
        check.setAttribute('role', 'switch');
}
for (const group of document.querySelectorAll('[data-choice-field]')) {
    group.onclick = event => {
        const b = event.target.closest('[data-value]');
        if (b && group.contains(b)) {
            const source = document.querySelector('[data-pref="' + group.dataset.choiceField + '"]');
            source.value = b.dataset.value;
            syncPreferenceChoices();
            source.dispatchEvent(new Event('change', {bubbles : true}));
        }
    };
    group.onkeydown = e => {
        if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(e.key))
            return;
        const choices = [...group.querySelectorAll('[data-value]:not(:disabled)')];
        const i = choices.indexOf(e.target);
        if (i < 0)
            return;
        e.preventDefault();
        const n = e.key === 'Home' ? 0
                  : e.key === 'End'
                      ? choices.length - 1
                      : (i + ([ 'ArrowLeft', 'ArrowUp' ].includes(e.key) ? -1 : 1) + choices.length) %
                            choices.length;
        choices[n].click();
        choices[n].focus();
    };
}
$('preferences-form').addEventListener('change', e => {
    if (e.target.matches('[data-pref],[data-network]')) {
        if (e.target.dataset.pref === 'purpose') {
            libraryCommand('setup',{mode:'purpose',purpose:Number(e.target.value)});
            return;
        }
        $('preferences-form').requestSubmit();
    }
});
$('reset-preferences').onclick = () => {
    const body = make(
        'p', tr('Ripristinare le impostazioni predefinite? Conversazioni e modelli rimangono disponibili.'));
    actionDialog(tr('Ripristina predefinite'), body, [
        button('Annulla', () => closeDialog($('action-dialog'))),
        button('Ripristina',
               () => {
                   send({command : 'preferences', resetGlobal : true, value : {}});
                   closeDialog($('action-dialog'));
               })
    ]);
};
window.overtonePreferencesSaved = () => {
    hideTip();
    if ($('preferences-dialog').open) {
        setFeedback($('preferences-error'));
        syncPreferenceChoices();
    }
    if ($('settings').open) updateConnectionFields();
    window.modelParametersSaved?.();
    waiting = false;
    controls()
};
function metadata(rows) {
    const dl = make('dl', '', 'metadata');
    for (const [label, value, id] of rows) {
        const row = make('div'), content = make('dd', String(value));
        if (id)
            content.id = id;
        row.append(make('dt', tr(label)), content);
        dl.append(row)
    }
    return dl
}
let locationStatus = null;
window.overtoneLocation = state => {
    locationStatus = state;
    const out = $('data-location-status');
    out.replaceChildren(metadata([
        ['Modalità', tr(state.custom ? 'Personalizzata' : 'Predefinita')],
        ['Cartella attuale', state.active],
        ['Al riavvio', state.pending || undefined],
        ['Dati da trasferire', state.pending ? sizes(state.bytes) : undefined]
    ].filter(row => row[1] !== undefined)));
    if (state.busy)
        out.append(make('p', tr('Verifica cartella')));
    if (state.error) {
        const message = {location_invalid : 'Scegli una cartella locale senza collegamenti e separata dai dati attuali.',
            location_readonly : 'La cartella non è scrivibile. Scegli un’altra posizione.',
            location_not_empty : 'La cartella deve essere vuota o appartenere già a questi dati OverTone.',
            location_inventory : 'Alcuni dati non sono leggibili o sono cambiati.',
            location_changed : 'La cartella scelta è cambiata. Selezionala di nuovo.',
            location_transfer : 'Trasferimento non completato. I dati originali sono conservati.'}[state.error];
        const feedback = make('p');
        setFeedback(feedback,message || explain(state.error),feedbackKind(state.error));
        out.append(feedback);
    }
    if (state.pending)
        out.append(make('p', tr('Il cambio sarà applicato al riavvio. La cartella originale sarà conservata.')));
    for (const b of document.querySelectorAll('[data-location-action]')) {
        const action = b.dataset.locationAction;
        b.disabled = state.busy || action === 'default' && !state.custom;
        b.hidden = ['cancel', 'restart'].includes(action) && !state.pending;
    }
};
for (const b of document.querySelectorAll('[data-location-action]'))
    b.onclick = () => send({command : 'location', action : b.dataset.locationAction});
function confirmModelRemoval(model) {
    const body = make('div');
    body.append(make(
        'p',
        tr(model.kind === 2 ? 'Puoi rimuovere il riferimento o disinstallare il modello da Ollama. I file condivisi con altri modelli vengono conservati dal servizio.'
           : model.canDelete
               ? 'Puoi rimuovere solo il riferimento oppure eliminare definitivamente anche il file GGUF dal computer.'
               : 'Verrà rimosso il riferimento da OverTone. Il file originale resta al suo posto.')));
    const remove = button('Rimuovi dalla lista', () => {
        closeDialog($('action-dialog'));
        libraryCommand('remove', {id : model.id})
    });
    remove.className = 'danger';
    const actions = [ button('Annulla', () => closeDialog($('action-dialog'))), remove ];
    if (model.canDelete) {
        const uninstall = button(model.kind === 2 ? 'Disinstalla da Ollama' : 'Disinstalla ed elimina file', () => {
            closeDialog($('action-dialog'));
            libraryCommand('remove', {id : model.id, uninstall : true});
        });
        uninstall.className = 'danger';
        actions.push(uninstall);
    }
    actionDialog(tr('Rimuovi') + ' ' + modelLabel(model) + '?', body, actions);
}
window.confirmDownload = (item, proceed) => {
    const body = make('div');
    const ollama = item.source === 'ollama';
    const installed = libraryState?.models.find(m => ollama ? m.kind === 2 && m.name === item.name :
        m.kind === 1 && item.checksum && m.metadata?.checksum === item.checksum);
    if (installed)
        body.append(make('p', tr(ollama ? 'Il modello è già installato. Il servizio aggiornerà la copia esistente.' :
            installed.managed ? 'Il modello è già installato. Il nuovo file sostituirà quello attuale dopo la verifica.' :
            'Il modello è già installato. La copia in libreria sarà sostituita; il file importato resterà nella sua posizione.'), 'download-replacement'));
    body.append(metadata([
        [ 'File', item.name ], [ 'Formato', ollama ? tr('Gestito dal servizio') : 'GGUF' ],
        [ 'Dimensione', (item.sizeApproximate ? '≈ ' : '') + sizes(item.bytes) ], [ 'Motore', ollama ? 'Ollama' : 'llama.cpp' ],
        [
            'Verifica SHA-256', ollama          ? tr('Gestita da Ollama')
                                : item.checksum ? tr('Disponibile')
                                                : tr('Non disponibile')
        ],
        [ 'Licenza', item.license || tr('Da verificare alla fonte') ]
    ]));
    const sourceURL = ollama ? 'https://ollama.com/library/' + encodeURIComponent(item.name) : item.sourceURL;
    if (sourceURL)
        body.append(
            button('Scheda del modello e licenza', () => send({command : 'openLink', url : sourceURL})));
    const yes = button(installed ? 'Scarica di nuovo' : 'Scarica', () => {
        closeDialog($('action-dialog'));
        proceed(installed ? {replaceId:installed.id} : {})
    });
    yes.className = 'primary';
    yes.id = 'download-confirm';
    yes.disabled = !ollama && !item.url;
    actionDialog(tr(installed ? 'Scarica di nuovo' : 'Scarica modello'), body, [ button('Annulla', () => closeDialog($('action-dialog'))), yes ]);
};
$('configure').onclick = () =>
    current?.connection.model
        ? modelParameters()
        : (navigate('models'), modelPage(libraryState?.models.length ? 'installed' : 'search'));
for (const b of document.querySelectorAll('[data-open-model-params]'))
    b.onclick = () => {
        modelParameters()
    };
const thinkingLabels = {
    auto : 'Automatico',
    off : 'Disattivato',
    on : 'Attivo',
    none : 'Disattivato',
    low : 'Basso',
    medium : 'Medio',
    high : 'Alto',
    minimal : 'Minimo',
    xhigh : 'Massimo',
    maximum : 'Massimo'
};
function thinkingOptions() {
    return ['auto', ...(current?.thinkingOptions || [])];
}
function thinkingDisplayOptions() {
    const values = thinkingOptions();
    return values.length > 1 && !values.some(x => ['off', 'none'].includes(x))
        ? [values[0], 'off', ...values.slice(1)] : values;
}
function thinkingLabel(value) {
    return tr(thinkingLabels[value] || String(value).replace(/[_-]/g, ' ').replace(/^./, c => c.toUpperCase()));
}
function effectiveThinking(options = thinkingOptions()) {
    return options.includes(current?.thinkingMode) ? current.thinkingMode : 'auto';
}
function closeThinkingPopover() {
    surfaceClose($('thinking-popover'));
    $('thinking-control').setAttribute('aria-expanded', 'false');
}
$('thinking-popover').addEventListener('focusout', e => {
    if (e.relatedTarget && !e.currentTarget.contains(e.relatedTarget))
        closeThinkingPopover();
});
$('thinking-control').onkeydown = e => {
    if ([ 'ArrowDown', 'ArrowUp' ].includes(e.key)) {
        e.preventDefault();
        if (!surfaceIsOpen($('thinking-popover')))
            $('thinking-control').click();
    }
};
$('thinking-control').setAttribute('popovertarget', 'thinking-popover');
$('thinking-control').onclick = event => {
    event.preventDefault();
    const p = $('thinking-popover');
    if (surfaceIsOpen(p)) {
        closeThinkingPopover();
        return;
    }
    if (!current)
        return;
    hideTip();
    closeModelPopover();
    p.replaceChildren();
    const options = thinkingOptions();
    const visibleOptions = thinkingDisplayOptions();
    for (const value of visibleOptions) {
        const text = thinkingLabel(value), selected = value === effectiveThinking(options);
        const b = button(text, () => {
            closeThinkingPopover();
            $('thinking-control').focus();
            send(current.selected ? {command : 'chatThinking', mode : value} : {
                command : 'preferences',
                value : {...current.preferences, thinkingMode : value, thinking : value !== 'off'}
            });
        });
        b.disabled = !options.includes(value);
        if (b.disabled)
            b.dataset.tip = tr('Non supportato dal modello');
        b.setAttribute('role', 'menuitemradio');
        b.dataset.thinking = value;
        b.setAttribute('aria-checked', String(selected));
        b.tabIndex = -1;
        b.replaceChildren(make('span', tr(text)), icon('check'));
        p.append(b);
    }
    surfaceOpen(p);
    positionSurface(p, $('thinking-control'));
    $('thinking-control').setAttribute('aria-expanded', 'true');
    hideTip();
    if (keyboardInput)
        p.querySelector('[aria-checked=true]')?.focus();
};
function chatAction(chat, kind) {
    if (waiting || current?.maintenance || current?.storageFailed)
        return;
    const run = () => {
        if (current.selected === chat.id) {
            if (kind === 'rename')
                openRename();
            else
                classifyChat(kind === 'restore' ? 0 : kind === 'archive' ? 1 : kind === 'delete' ? 3 : 2);
            return
        }
        endEdit(true);
        flushDraft();
        pendingChatAction = {id : chat.id, kind};
        waiting = true;
        controls();
        send({command : 'select', id : chat.id});
    };
    if (kind === 'trash' || kind === 'delete') {
        const body = make('div');
        body.append(make('p', chat.title));
        const yes = button(kind === 'delete' ? 'Elimina definitivamente' : 'Sposta nel cestino', () => {
            closeDialog($('action-dialog'));
            run()
        });
        yes.className = 'danger';
        actionDialog(tr(kind === 'delete' ? 'Elimina definitivamente' : 'Sposta nel cestino'), body,
                     [ button('Annulla', () => closeDialog($('action-dialog'))), yes ]);
    } else
        run();
}
window.finishPendingChatAction = state => {
    if (!pendingChatAction || state.selected !== pendingChatAction.id)
        return;
    const action = pendingChatAction;
    pendingChatAction = null;
    if (action.kind === 'rename')
        openRename();
    else
        classifyChat(action.kind === 'restore' ? 0 : action.kind === 'archive' ? 1 : action.kind === 'delete' ? 3 : 2)
};
window.failPendingChatAction = () => { pendingChatAction = null };
// A bounded presentation window over native SQL pages; no second conversation store.
const conversationPages = new Map();
let openConversation = null;
const outsideConversation = make('section', '', 'outside-conversation');
outsideConversation.hidden = true;
$('chats').before(outsideConversation);
let conversationListKey = '', conversationMutation = '', conversationTotal = 0, conversationState = null;
let conversationRequest = -1, conversationFocus = -1, conversationFrame = 0, conversationViewport = null;
function conversationRowHeight() {
    return conversationState?.query ? 132 : 102;
}
window.resetConversationWindow = () => {
    conversationPages.clear();
    conversationListKey = '';
    conversationRequest = -1;
    $('chats').scrollTop = 0;
};
function requestConversationPage(offset) {
    if (conversationRequest >= 0 || offset < 0 || offset >= conversationTotal)
        return;
    conversationRequest = offset;
    $('chats').setAttribute('aria-busy', 'true');
    send({command : 'search', query : $('search').value, before : offset});
}
function buildConversationRow(chat, state, index, existing = null) {
    const signature = JSON.stringify([
        chat, chat.id === state.selected, !!(state.active && state.activeChat === chat.id),
        !!waiting, state.maintenance, state.storageFailed, state.filter, !!state.query, language, formatRevision
    ]);
    if (existing?.dataset.signature === signature) return existing;
    const row = make('article', '', 'conversation-row');
    row.dataset.signature = signature;
    row.dataset.chatId = chat.id;
    row.dataset.index = index;
    row.setAttribute('aria-current', String(chat.id === state.selected));
    const locked = state.maintenance || state.storageFailed;
    const open = button(chat.title, () => {
        endEdit(true);
        flushDraft();
        window.transcriptView.jump(chat.id, chat.match);
        send({command : 'select', id : chat.id, match : chat.match});
        setHistoryOpen(false);
    }, true);
    open.className = 'conversation-open';
    open.disabled = locked;
    open.setAttribute('aria-label', tr('Apri') + ': ' + chat.title);
    row.append(open);
    row.onclick = e => {
        if (!e.target.closest('button') && !open.disabled)
            open.click()
    };
    const stamp = make('time');
    setTimestamp(stamp, chat.updated);
    row.append(stamp);
    const excerpt = (chat.snippet || '').trim();
    const snippet =
        make('p', excerpt.toLocaleLowerCase() === chat.title.trim().toLocaleLowerCase() ? '' : excerpt,
             'conversation-snippet');
    row.append(snippet);
    const bottom = make('div', '', 'row'), count = make('small', chat.messages === undefined ? '' : String(chat.messages)),
          flags = make('span', '', 'conversation-flags');
    if (chat.messages !== undefined) {
        count.prepend(icon('messages-square'));
        count.setAttribute('aria-label', quantity('messages', chat.messages));
    }
    flags.append(count);
    if (state.active && state.activeChat === chat.id) {
        const running = make('span','','conversation-running');
        running.append(icon('loader'));
        running.dataset.tip = tr('Generazione in corso');
        running.setAttribute('role','img');
        running.setAttribute('aria-label',running.dataset.tip);
        row.classList.add('generating');
        row.append(running);
    } else if (chat.unread) {
        const unread = make('span', '', 'conversation-unread');
        unread.setAttribute('role', 'img');
        unread.setAttribute('aria-label', tr('Risposta da leggere'));
        unread.dataset.tip = tr('Risposta da leggere');
        row.classList.add('unread');
        row.append(unread);
    }
    if (state.query) {
        const match = make('span', '', 'conversation-flag'),
              label = tr(chat.match ? 'Corrispondenza nel messaggio' : 'Corrispondenza nel titolo');
        match.append(icon(chat.match ? 'messages-square' : 'search'));
        match.dataset.tip = label;
        match.setAttribute('aria-label', label);
        match.setAttribute('role', 'img');
        flags.append(match);
    }
    for (const [key, label, glyph] of [[ 'code', 'Con codice', 'code' ],
                                       [ 'errors', 'Con errori', 'triangle-alert' ],
                                       [ 'compacted', 'Contesto riassunto', 'memory-stick' ]]) {
        if (!chat[key])
            continue;
        const badge = make('span', '', 'conversation-flag');
        badge.append(icon(glyph));
        badge.dataset.tip = tr(label);
        badge.setAttribute('role', 'img');
        badge.setAttribute('aria-label', tr(label));
        flags.append(badge);
    }
    bottom.classList.add('conversation-bottom');
    const actions = make('div', '', 'row conversation-actions');
    const move = actionButton('Sposta nel progetto · non ancora disponibile', 'folder-open', () => {});
    move.disabled = true;
    const rename =
        actionButton('Rinomina titolo', 'pencil', () => chatAction(chat, 'rename'));
    const trash =
        actionButton(state.filter === 2 ? 'Elimina definitivamente' : 'Sposta nel cestino',
                     'trash', () => chatAction(chat, state.filter === 2 ? 'delete' : 'trash'));
    rename.disabled = trash.disabled = !!(locked || waiting);
    actions.append(move,rename);
    if (state.filter !== 1) {
        const archive = actionButton('Archivia','archive', () => chatAction(chat,'archive'));
        archive.disabled = !!(locked || waiting);
        actions.append(archive);
    }
    if (state.filter) {
        const recover = actionButton('Ripristina conversazione','rotate-ccw', () => chatAction(chat,'restore'), 'constructive');
        recover.disabled = !!(locked || waiting);
        actions.append(recover);
    }
    trash.classList.add('danger');
    actions.append(trash);
    bottom.append(flags, actions);
    row.append(bottom);
    return row;
}
function paintConversationWindow() {
    conversationFrame = 0;
    const list = $('chats'), content = $('conversation-list'), state = conversationState;
    if (!state)
        return;
    if (!conversationTotal) {
        conversationViewport = null;
        content.replaceChildren(make('p',
                                  tr(state.query          ? 'Nessun risultato con questi filtri.'
                                     : state.filter === 1 ? 'Archivio vuoto'
                                     : state.filter === 2 ? 'Cestino vuoto'
                                                          : 'Nessuna conversazione'),
                                  'list-empty'));
        return;
    }
    const height = list.clientHeight || 400, top = list.scrollTop, stride = conversationRowHeight(),
          inset = parseFloat(getComputedStyle(list).paddingTop) || 0;
    const first = Math.max(0, Math.floor((top - inset) / stride) - 3);
    const last = Math.min(conversationTotal, first + Math.ceil(height / stride) + 7);
    conversationViewport = {top,height,first,last,total:conversationTotal};
    const active = document.activeElement, focus = active?.closest('.conversation-row');
    const focusedLabel = active?.getAttribute('aria-label');
    const focusedIndex = focus ? Number(focus.dataset.index) : -1;
    const existing =
        new Map([...list.querySelectorAll('.conversation-row') ].map(r => [Number(r.dataset.index), r]));
    const nodes = [];
    const spacer = (n, edge) => {
        const e = list.querySelector('[data-space="' + edge + '"]') || make('div');
        e.dataset.space = edge;
        e.style.height = n + 'px';
        e.setAttribute('aria-hidden', 'true');
        return e;
    };
    nodes.push(spacer(first * stride, 'start'));
    let missing = -1;
    for (let i = first; i < last; i++) {
        const offset = Math.floor(i / 30) * 30, page = conversationPages.get(offset),
              chat = page?.[i - offset];
        if (!chat) {
            if (missing < 0)
                missing = offset;
            const skeleton = make('div', '', 'conversation-skeleton');
            skeleton.style.height = stride + 'px';
            nodes.push(skeleton);
            continue;
        }
        const row = buildConversationRow(chat, state, i, existing.get(i));
        row.style.height = (stride - 6) + 'px';
        nodes.push(row);
    }
    nodes.push(spacer((conversationTotal - last) * stride, 'end'));
    // Retain rows in place. Detaching them all on every scroll loses focus and hover.
    let cursor = content.firstChild;
    for (const node of nodes) {
        if (node === cursor) cursor = cursor.nextSibling;
        else content.insertBefore(node, cursor);
    }
    const retained = new Set(nodes);
    for (const child of [...content.children])
        if (!retained.has(child)) child.remove();
    if (conversationFocus >= first && conversationFocus < last) {
        const target = list.querySelector('[data-index="' + conversationFocus + '"] .conversation-open');
        if (target) {
            conversationFocus = -1;
            target.focus({preventScroll : true})
        }
    } else if (!active?.isConnected && focusedIndex >= first && focusedIndex < last) {
        const row = list.querySelector('[data-index="' + focusedIndex + '"]'),
              replacement = [...(row?.querySelectorAll('button:not(:disabled)') || [])]
                  .find(button => button.getAttribute('aria-label') === focusedLabel);
        (replacement || row?.querySelector('.conversation-open'))?.focus({preventScroll:true});
    }
    if (missing >= 0)
        requestConversationPage(missing);
}
$('chats').addEventListener('scroll', () => {
    if (!conversationFrame)
        conversationFrame = requestAnimationFrame(paintConversationWindow)
}, {passive : true});
new ResizeObserver(() => {
    if (!conversationFrame)
        conversationFrame = requestAnimationFrame(paintConversationWindow)
}).observe($('chats'));
$('chats').addEventListener('keydown', e => {
    const row = e.target.closest('.conversation-row');
    if (!row || !e.target.classList.contains('conversation-open') ||
        !['ArrowUp', 'ArrowDown', 'Home', 'End'].includes(e.key))
        return;
    e.preventDefault();
    conversationFocus = Math.max(
        0, Math.min(conversationTotal - 1,
                    e.key === 'Home'  ? 0
                    : e.key === 'End' ? conversationTotal - 1
                                      : Number(row.dataset.index) + (e.key === 'ArrowDown' ? 1 : -1)));
    const list = $('chats'), stride = conversationRowHeight(),
          top = (parseFloat(getComputedStyle(list).paddingTop) || 0) + conversationFocus * stride;
    if (top < list.scrollTop)
        list.scrollTop = top;
    else if (top + stride > list.scrollTop + list.clientHeight)
        list.scrollTop = top + stride - list.clientHeight;
    paintConversationWindow();
});
window.renderConversationList = (state, reveal = false) => {
    if (state.query !== $('search').value.trim())
        return;
    const cached = [...conversationPages.values()].flat(),
          selected = state.chats.find(chat => chat.id === state.selected) || cached.find(chat => chat.id === state.selected);
    if (selected)
        openConversation = selected;
    else if (openConversation?.id !== state.selected)
        openConversation = null;
    const key = JSON.stringify([ state.query, state.filter, appliedChatFilters, language ]);
    const mutation =
        JSON.stringify([ state.selected, state.head, state.title, state.active, state.chatTotal ]);
    if (key !== conversationListKey) {
        conversationPages.clear();
        $('chats').scrollTop = 0;
        conversationListKey = key;
    } else if (mutation !== conversationMutation)
        conversationPages.clear();
    conversationMutation = mutation;
    conversationState = state;
    conversationTotal = state.chatTotal || 0;
    conversationRequest = -1;
    conversationPages.delete(state.chatOffset || 0);
    conversationPages.set(state.chatOffset || 0, state.chats);
    while (conversationPages.size > 16)
        conversationPages.delete(conversationPages.keys().next().value);
    // A viewport cache is not membership. Paging must not insert/remove a pinned
    // card and resize the scroll viewport as the selected row enters its cache.
    const classification = state.deleted ? 2 : state.archived ? 1 : 0;
    outsideConversation.hidden = !state.selected || state.filter === classification;
    if (!outsideConversation.hidden) {
        const active = {...openConversation, id : state.selected, title : state.title, match : 0},
              activeState = {...state, query : '', filter : state.deleted ? 2 : state.archived ? 1 : 0};
        const old = outsideConversation.querySelector('.conversation-row'),
              row = buildConversationRow(active, activeState, -1, old);
        if (!old) outsideConversation.append(make('small',tr('Conversazione aperta')),row);
        else if (row !== old) old.replaceWith(row);
        const label = outsideConversation.firstElementChild, text = tr('Conversazione aperta');
        if (label.textContent !== text) label.textContent = text;
    }
    $('chats').setAttribute('aria-busy', 'false');
    // Establish the destination extent before scrolling; the old (possibly one-row)
    // history would clamp the selected item's scroll position to the previous range.
    const extent = conversationTotal * conversationRowHeight() + 'px';
    if ($('conversation-list').style.minHeight !== extent)
        $('conversation-list').style.minHeight = extent;
    if (reveal)
        $('chats').scrollTop = Math.max(0, state.chatRevealIndex * conversationRowHeight());
    paintConversationWindow();
};
window.renderVariants = (row, turn, state) => {
    const previous = row.role === 'user' ? turn.userPrevious : turn.previous,
          following = row.role === 'user' ? turn.userNext : turn.next,
          versions = row.role === 'user' ? turn.userVersions : turn.versions,
          variantIndex = row.role === 'user' ? turn.userVariantIndex : turn.variantIndex;
    const signature = JSON.stringify([
        previous, following, versions, variantIndex, state.active, state.improving,
        state.maintenance, state.storageFailed, language
    ]);
    if (row.variantSignature === signature)
        return;
    row.variantSignature = signature;
    row.header.querySelector('.variant-navigation')?.remove();
    if (versions <= 1)
        return;
    const nav = make('div', '', 'variant-navigation');
    nav.setAttribute('role', 'group');
    nav.setAttribute('aria-label', tr(row.role === 'user' ? 'Versioni del messaggio' : 'Versioni della risposta'));
    const prev = actionButton('Versione precedente', 'chevron-left', () => {
        endEdit(true);
        send({command : 'choose', id : previous})
    }), next = actionButton('Versione successiva', 'chevron-right', () => {
            endEdit(true);
            send({command : 'choose', id : following})
        });
    prev.disabled = !previous || state.maintenance || state.storageFailed;
    next.disabled = !following || state.maintenance || state.storageFailed;
    const index = variantIndex || 1, count = make('small', index + ' / ' + versions);
    nav.append(prev, count, next);
    row.header.append(nav);
};
let historyFilterMotion = null;
function setHistoryFiltersOpen(open) {
    const started = performance.now(), panel = $('chat-filters'), list = $('history-body'),
          reader = $('chats'), root = $('history-rail');
    if (historyFilterMotion?.open === open) return;
    const {slot,clip} = revealContainers(panel),
          oldHeight = clip.offsetHeight,
          shown = panel.hidden ? 0 : Math.max(0,oldHeight + new DOMMatrixReadOnly(getComputedStyle(clip).transform).m42),
          before = list.getBoundingClientRect(), scrollTop = reader.scrollTop;
    historyFilterMotion?.clean();
    panel.hidden = panel.inert = !open;
    if (!motionEnabled() || hiddenUIElement(root)) return;
    panel.hidden = false;
    const height = open ? clip.offsetHeight : oldHeight;
    slot.classList.toggle('collapsing',!open);
    if (!open) clip.style.height = height+'px';
    const after = list.getBoundingClientRect(), origin = root.getBoundingClientRect(),
          keys = ['position','left','top','width','height','flex'],
          saved = Object.fromEntries(keys.map(key => [key,list.style[key]]));
    // The scroll viewport is clipped by the rail while it moves. Keep its larger
    // height until settlement: shrinking it first would cut off moving rows.
    Object.assign(list.style,{position:'absolute',left:after.left-origin.left-root.clientLeft+'px',
        top:after.top-origin.top-root.clientTop+'px',width:after.width+'px',
        height:Math.max(before.height,after.height)+'px',flex:'none'});
    const scrollShift = reader.scrollTop-scrollTop,
          timing = {...motionTiming(open ? 'panel' : 'panel-exit'),fill:'both'},
          effects = [
            ...revealEffects(clip,panel,shown,height,open,timing),
            list.animate([{transform:`translateY(${before.top-after.top}px)`},{transform:'none'}],timing)
          ];
    if (scrollShift)
        effects.push($('conversation-list').animate(
            [{transform:`translateY(${scrollShift}px)`},{transform:'none'}],timing));
    const movement = historyFilterMotion = {open,clean:() => {
        if (historyFilterMotion !== movement) return;
        historyFilterMotion = null;
        for (const effect of effects) effect.cancel();
        Object.assign(list.style,saved);
        panel.hidden = panel.inert = !open;
        slot.classList.remove('collapsing');
        clip.style.height = '';
        scheduleChromeTargets();
    }};
    for (const effect of effects) effect.finished.catch(() => {});
    measureMotion('filters',effects[0],performance.now()-started);
    effects[0].finished.then(movement.clean,() => {});
}
window.addEventListener('resize',() => historyFilterMotion?.clean());
$('chat-filters-toggle').onclick = () => {
    hideTip();
    const open = $('chat-filters-toggle').getAttribute('aria-expanded') !== 'true';
    setHistoryFiltersOpen(open);
    $('chat-filters-toggle').setAttribute('aria-expanded', String(open))
};
let appliedChatFilters = {}, dateShortcut = 0;
function renderChatFilterChips() {
    const out = $('active-chat-filters');
    out.replaceChildren();
    const add = (label, clear) => {
        const chip = button(label, () => {
            clear();
            applyChatFilters()
        });
        chip.className = 'filter-chip';
        chip.append(icon('x'));
        chip.setAttribute('aria-label', tr('Rimuovi filtro') + ': ' + label);
        out.append(chip);
    };
    if ($('chat-from').value || $('chat-to').value)
        add(dateShortcut ? tr(dateShortcut === 7 ? 'Ultimi 7 giorni' : 'Ultimi 30 giorni')
                         : [ $('chat-from').value, $('chat-to').value ].filter(Boolean).join(' – '),
            () => {
                $('chat-from').value = $('chat-to').value = '';
                dateShortcut = 0;
            });
    for (const id of ['chat-code', 'chat-errors', 'chat-compacted'])
        if ($(id).checked)
            add($(id).parentElement.textContent.trim(), () => $(id).checked = false);
    if ($('chat-min').value || $('chat-max').value)
        add(tr('Messaggi') + ': ' + ($('chat-min').value || '0') + '–' + ($('chat-max').value || '∞'),
            () => {$('chat-min').value = $('chat-max').value = ''});
    for (const choice of document.querySelectorAll(
             '#chat-model-options input:checked,#chat-runtime-options input:checked'))
        add(choice.value, () => choice.checked = false);
}
function applyChatFilters() {
    for (const field of [$('chat-from'), $('chat-to')])
        field.dataset.empty = String(!field.value);
    const from = $('chat-from').value ? new Date($('chat-from').value + 'T00:00:00') : null;
    const to = $('chat-to').value ? new Date($('chat-to').value + 'T00:00:00') : null;
    if (to)
        to.setDate(to.getDate() + 1);
    const minimum = $('chat-min').value ? Number($('chat-min').value) : 0;
    const maximum = $('chat-max').value ? Number($('chat-max').value) : undefined;
    const dateValid = (!from || Number.isFinite(+from)) && (!to || Number.isFinite(+to)) &&
        (!from || !to || +from < +to);
    const countValid = Number.isInteger(minimum) && minimum >= 0 && minimum <= 10000000 &&
        (maximum === undefined || Number.isInteger(maximum) && maximum >= minimum && maximum <= 10000000);
    for (const [id, valid] of [['chat-from', dateValid], ['chat-to', dateValid],
                              ['chat-min', countValid], ['chat-max', countValid]]) {
        $(id).setAttribute('aria-invalid', String(!valid));
        if (valid)
            $(id).removeAttribute('aria-describedby');
        else
            $(id).setAttribute('aria-describedby', 'chat-filter-error');
    }
    const models = [...document.querySelectorAll('#chat-model-options input:checked') ].map(x => x.value);
    const runtimes = [...document.querySelectorAll('#chat-runtime-options input:checked') ].map(x => x.value);
    setFeedback($('chat-filter-error'),
        !dateValid ? 'Controlla l’intervallo delle date.'
        : !countValid ? maximum !== undefined && maximum < minimum
            ? 'Il massimo è inferiore al minimo.' : 'Controlla il numero di messaggi.'
        : models.length > 16 || runtimes.length > 8 ? 'Troppi filtri selezionati.' : '', 'warning');
    if ($('chat-filter-error').textContent)
        return;
    appliedChatFilters = {
        sort : Number($('chat-sort').value),
        minimum,
        maximum,
        from : from ? Math.floor(+from / 1000) : 0,
        until : to ? Math.floor(+to / 1000) : 0,
        code : $('chat-code').checked,
        errors : $('chat-errors').checked,
        compacted : $('chat-compacted').checked,
        models,
        runtimes
    };
    for (const b of document.querySelectorAll('[data-date-days]'))
        b.setAttribute('aria-pressed', String(Number(b.dataset.dateDays) === dateShortcut));
    window.resetConversationWindow?.();
    send({command : 'searchFilters', value : appliedChatFilters});
    renderChatFilterChips();
}
for (const field of $('chat-filters').querySelectorAll('input,select'))
    field.addEventListener('change', () => {
        if (field.type === 'date')
            dateShortcut = 0;
        applyChatFilters()
    });
for (const b of document.querySelectorAll('[data-date-days]'))
    b.onclick = () => {
        const days = Number(b.dataset.dateDays);
        if (dateShortcut === days) {
            dateShortcut = 0;
            $('chat-from').value = $('chat-to').value = ''
        } else {
            dateShortcut = days;
            const end = new Date(), start = new Date();
            start.setDate(start.getDate() - days + 1);
            const iso = d => [d.getFullYear(), String(d.getMonth() + 1).padStart(2, '0'),
                              String(d.getDate()).padStart(2, '0')]
                                 .join('-');
            $('chat-from').value = iso(start);
            $('chat-to').value = iso(end);
        }
        applyChatFilters();
    };
window.renderChatFilterOptions = state => {
    for (const [kind, items] of [[ 'model', state.chatModels || [] ],
                                 [ 'runtime', state.chatRuntimes || [] ]]) {
        const out = $('chat-' + kind + '-options'), signature = JSON.stringify([ items, language ]);
        $('chat-' + kind + '-filters').hidden = !items.length;
        if (out.dataset.signature === signature)
            continue;
        out.dataset.signature = signature;
        const selected = appliedChatFilters[kind === 'model' ? 'models' : 'runtimes'] || [];
        const existing = new Map([...out.children ].map(x => [x.dataset.value, x]));
        for (const item of items) {
            let label = existing.get(item.value);
            if (!label) {
                label = make('label', '', 'check filter-choice');
                label.dataset.value = item.value;
                const input = make('input');
                input.type = 'checkbox';
                input.value = item.value;
                input.checked = selected.includes(item.value);
                input.onchange = applyChatFilters;
                label.append(input, make('span'), make('small'));
            }
            label.querySelector('span').textContent = kind !== 'runtime' ? item.value
                : item.value === 'openai' ? tr('API compatibile con OpenAI')
                : item.value === 'ollama' ? 'Ollama' : item.value === 'llama' ? 'llama.cpp' : item.value;
            const count = label.lastElementChild;
            count.replaceChildren(icon('messages-square'), document.createTextNode(String(item.count)));
            count.dataset.tip = quantity('conversations', item.count);
            count.setAttribute('aria-label', count.dataset.tip);
            out.append(label);
            existing.delete(item.value);
        }
        for (const label of existing.values())
            label.remove();
    }
    $('chat-sort').querySelector('[value="6"]').disabled = !state.query;
    if (!state.query && $('chat-sort').value === '6') {
        $('chat-sort').value = '0';
        appliedChatFilters.sort = 0;
    }
};
function resetChatFilters(update = true) {
    for (const n of $('chat-filters').querySelectorAll('input'))
        n.type === 'checkbox' ? n.checked = false : n.value = '';
    $('chat-sort').value = '0';
    dateShortcut = 0;
    if (update) applyChatFilters();
    else {
        appliedChatFilters = {};
        setFeedback($('chat-filter-error'));
        for (const field of $('chat-filters').querySelectorAll('input')) {
            field.removeAttribute('aria-invalid');
            field.removeAttribute('aria-describedby');
            if (field.type === 'date') field.dataset.empty = 'true';
        }
        for (const button of document.querySelectorAll('[data-date-days]'))
            button.setAttribute('aria-pressed','false');
        renderChatFilterChips();
    }
}
$('chat-filter-reset').onclick = () => resetChatFilters();
function preparedCard(title, glyph, rows) {
    const card = make('section', '', 'prepared-card flow-surface'), h = make('h2', tr(title));
    h.prepend(icon(glyph));
    card.append(h, metadata(rows.map(([ a, b ]) => [tr(a), tr(b)])));
    return card
}
let toolSection = 'tools';
const toolSections = [
    [ 'tools', 'Tool', 'blocks', 'Richiedi tool' ], [ 'plugins', 'Plugin', 'box', 'Aggiungi plugin' ],
    [ 'mcp', 'Server MCP', 'cloud', 'Collega server MCP' ],
    [ 'skills', 'Skill', 'book-open', 'Importa skill' ],
    [ 'automations', 'Automazioni', 'workflow', 'Nuova automazione' ]
];
function renderPrepared(id) {
    const data = preparedViews[id];
    if (!data)
        return;
    hideTip();
    $('contextual-title').textContent = tr(data.title);
    $('prepared-search').placeholder = tr(data.search);
    $('prepared-search').setAttribute('aria-label', tr(data.search));
    const rail = $('contextual-items');
    rail.replaceChildren();
    function toolNavigation(target, compact = false) {
        for (const [key, label, glyph] of toolSections) {
            const item = button(label, () => {
                toolSection = key;
                renderPrepared('tools');
            });
            item.prepend(icon(glyph));
            item.className = 'contextual-navigation';
            item.setAttribute('aria-current', String(key === toolSection));
            if (compact) {
                item.replaceChildren(icon(glyph), make('span', tr(label)));
                item.dataset.tip = tr(label);
                item.setAttribute('aria-label', tr(label));
            }
            target.append(item);
        }
    }
    if (id === 'tools')
        toolNavigation(rail);
    else
        for (const [label, detail] of data.rail) {
            const item = make('section', '', 'contextual-item');
            item.append(make('h3', tr(label)), make('p', tr(detail)));
            rail.append(item)
        }
    const main = $('prepared-content');
    main.replaceChildren();
    const head = make('header', '', 'page-heading flow-surface'), titles = make('div');
    titles.append(make('small', tr(data.eyebrow)), make('h1', tr(data.title)));
    head.append(titles);
    main.append(head);
    if (id === 'tools') {
        const navigation = make('nav', '', 'compact-tool-navigation');
        navigation.setAttribute('aria-label', tr('Strumenti'));
        toolNavigation(navigation, true);
        main.append(navigation);
    }
    if (id === 'tools' && toolSection !== 'tools') {
        const [, label, glyph, actionLabel] = toolSections.find(x => x[0] === toolSection),
                               empty = make('section', '', 'prepared-empty'),
                               action = button(actionLabel, () => {});
        empty.append(icon(glyph), make('h2', tr(label)), make('p', tr('Non ancora disponibile')));
        action.classList.add('constructive');
        action.disabled = true;
        empty.append(action);
        main.append(empty);
        return;
    }
    const grid = make('div', '', 'prepared-grid');
    for (const c of data.cards) {
        const card = preparedCard(c.title, c.icon, c.rows || []);
        for (const t of c.toggles || []) {
            const row = make('div', '', 'prepared-toggle'), copy = make('div');
            copy.append(make('h3', tr(t[0])), make('p', tr(t[1])));
            const sw = make('span', '', 'prepared-switch');
            sw.setAttribute('role', 'switch');
            sw.setAttribute('aria-checked', 'false');
            sw.setAttribute('aria-disabled', 'true');
            sw.setAttribute('aria-label', tr(t[0]));
            row.append(copy, sw);
            card.append(row)
        }
        grid.append(card)
    }
    main.append(grid);
    const bottom = make('section', '', 'prepared-bottom flow-surface');
    const states = make('div', '', 'row');
    states.append(...data.states.map(text => make('span', tr(text), 'badge')));
    bottom.append(make('h2', tr(data.bottom)), states);
    const action = button(data.action, () => {});
    action.classList.add('constructive');
    action.disabled = true;
    bottom.append(make('div', ''), action);
    main.append(bottom);
}
onUIEvent('keydown', e => {
    if (e.defaultPrevented) return;
    if (e.key === 'Escape') {
        hideTip();
        if (surfaceIsOpen($('thinking-popover'))) {
            e.preventDefault();
            closeThinkingPopover();
            $('thinking-control').focus();
            hideTip();
            return;
        }
        if (surfaceIsOpen($('model-popover'))) {
            e.preventDefault();
            closeModelPopover();
            $('model-menu').focus();
            hideTip();
            return;
        }
        const surface = [...document.querySelectorAll('[popover=auto],[data-fallback-popover=auto]')]
            .find(surfaceIsOpen);
        if (surface) {
            e.preventDefault();
            surfaceClose(surface);
            document.querySelector('[popovertarget="' + CSS.escape(surface.id) + '"]')?.focus();
            hideTip();
            return;
        }
        if (!document.querySelector('dialog[open]')) {
            if ($('chat-filters-toggle').getAttribute('aria-expanded') === 'true' &&
                !e.target.matches('select,input[type=date]')) {
                e.preventDefault();
                $('chat-filters-toggle').click();
                $('chat-filters-toggle').focus();
                return;
            }
            closeContextPanel();
        }
    }
    const tabs = e.target.closest('[role=tablist]');
    if (tabs && [ 'ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End' ].includes(e.key)) {
        const all = [...tabs.querySelectorAll('[role=tab]') ], i = all.indexOf(e.target);
        if (i < 0)
            return;
        e.preventDefault();
        const n =
            e.key === 'Home' ? 0
            : e.key === 'End'
                ? all.length - 1
                : (i + ([ 'ArrowRight', 'ArrowDown' ].includes(e.key) ? 1 : -1) + all.length) % all.length;
        all[n].click();
        all[n].focus();
    }
});
window.refreshShellLanguage = () => {
    if (locationStatus)
        window.overtoneLocation(locationStatus);
    if (!['chat', 'models'].includes(route))
        renderPrepared(route);
    const b = $('area-label');
    b.textContent = tr({
        chat : 'Chat',
        models : 'Modelli',
        knowledge : 'Documenti',
        projects : 'Progetti',
        tools : 'Strumenti'
    }[route]);
};
window.refreshRegionalViews = () => {
    if (!current) return;
    window.overtoneReceive(current);
    window.refreshLibrary?.();
    window.renderDetails?.();
};
window.addEventListener('resize', () => {
    closeFloatingSurfaces([$('feature-guide'),$('guide-highlight')]);
    clearDownloadMotion();
    downloadLayout(downloadWanted);
    $('chat-workspace').hidden = route !== 'chat' && innerWidth < 1568;
    detailsTransition?.finish();
    for (const surface of uiElements('[data-surface-motion]'))
        surface.surfaceAnimation?.finish();
    closeModelPopover();
    closeThinkingPopover();
    setHistoryOpen(false);
    updateComposer()
});

const preparedViews = {
    "knowledge" : {
        "title" : "Documenti",
        "eyebrow" : "Libreria",
        "search" : "Cerca documenti",
        "rail" : [
            [ "Sezioni", "Albero cartelle" ], [ "Indice parziale", "Indicizzazione" ],
            [ "RAG", "RAG inattivo" ]
        ],
        "cards" : [
            {
                "title" : "Documenti indicizzati",
                "icon" : "library",
                "rows" : [
                    [ "Documenti", "0" ], [ "Documenti indicizzati", "0" ], [ "Chunk", "0" ],
                    [ "Parser predefinito", "Non disponibile" ]
                ]
            },
            {
                "title" : "Pagine web salvate",
                "icon" : "globe",
                "rows" : [
                    [ "Snapshot locali", "0" ], [ "Ricerca online", "Non attivo" ], [ "RAG", "RAG inattivo" ]
                ]
            }
        ],
        "bottom" : "Import e reindex",
        "states" : [ "Import disabilitato", "Reindex", "RAG · Non attivo" ],
        "action" : "Import"
    },
    "projects" : {
        "title" : "Progetti",
        "eyebrow" : "Workspace",
        "search" : "Cerca progetti",
        "rail" : [
            [ "Modalità progetto", "Scope progetto" ], [ "Memoria progetto", "Predisposto" ],
            [ "Tools", "Non attivo" ]
        ],
        "cards" : [
            {
                "title" : "Progetti",
                "icon" : "folder-open",
                "rows" : [
                    [ "Scope progetto", "Non disponibile" ], [ "Modello", "Non disponibile" ],
                    [ "Runtime", "Non disponibile" ], [ "Stato", "Predisposto" ]
                ]
            },
            {
                "title" : "Memoria progetto",
                "icon" : "memory-stick",
                "toggles" : [
                    [ "Preferire fonti ufficiali", "Origine: chat locale" ],
                    [ "Report Markdown con sezione limiti", "Origine: artifact generato" ]
                ]
            }
        ],
        "bottom" : "Project operations",
        "states" : [ "Apri progetto", "Memoria progetto", "Tools · Non attivo" ],
        "action" : "Nuovo progetto"
    },
    "tools" : {
        "title" : "Tools",
        "eyebrow" : "Tool policy",
        "search" : "Cerca tool",
        "rail" : [
            [ "Tool locali", "Predisposto" ], [ "Shell/Python", "Shell/Python non attivo" ],
            [ "Policy approvazioni", "Audit" ]
        ],
        "cards" : [
            {
                "title" : "Tool locali",
                "icon" : "blocks",
                "rows" : [
                    [ "Search KB", "Predisposto" ], [ "Create note", "Predisposto" ],
                    [ "Export conversation", "Predisposto" ], [ "Shell", "Non attivo" ],
                    [ "Python", "Non attivo" ]
                ]
            },
            {
                "title" : "API configurate",
                "icon" : "cloud",
                "rows" : [
                    [ "OpenAI-compatible locale", "Predisposto" ],
                    [ "Provider remoto personale", "Non attivo" ], [ "MCP", "Predisposto" ],
                    [ "API server locale", "Non attivo" ]
                ]
            }
        ],
        "bottom" : "Esecuzione tool inattiva",
        "states" : [ "Esecuzione tool inattiva", "Shell/Python non attivo", "Policy approvazioni" ],
        "action" : "Richiedi tool"
    }
};
// AppKit owns the window chrome; only the visible hit regions come from layout.
function scheduleChromeTargets() {
    if (chromeFrame || document.documentElement.dataset.chrome !== 'mac')
        return;
    chromeFrame = true;
    // Native hit regions must update even when WebKit suspends animation frames while occluded.
    queueMicrotask(() => {
        chromeFrame = false;
        const started = performance.now();
        const header = document.querySelector('.shell-topbar');
        const rects =
            [...header.querySelectorAll('button, input, select, a, .top-navigation, .model-control') ]
                .map(n => n.getBoundingClientRect())
                .filter(r => r.width > 0 && r.height > 0)
                .map(r => [r.x, r.y, r.width, r.height]);
        rects.push(...chromeMotionBounds.values());
        const signature = JSON.stringify([ innerWidth, innerHeight, rects ]);
        const scrimStyle = getComputedStyle($('context-scrim'));
        const metrics = {
            rendererFocused: Number(document.hasFocus()),
            rendererWindowActive: Number(window.overtoneWindowActive !== false),
            rendererHidden: Number(document.hidden),
            rendererMotionAllowed: Number(motionEnabled()),
            rendererFrameRateAPI: Number('frameRate' in Animation.prototype),
            rendererMaximumFrameRate: Number(document.timeline.maximumFrameRate) || 0,
            rendererHighRateCSS: Number(CSS.supports('animation-frame-rate', 'highest')),
            rendererOverlayScrollbars: Number(document.documentElement.dataset.overlayScrollbars === 'true'),
            rendererAnimations: uiAnimations().filter(a =>
                scriptMotion(a) &&
                Number.isFinite(a.effect?.getComputedTiming().endTime) && a.playState !== 'finished').length,
            contextScrimOpen: Number($('context-scrim').classList.contains('open')),
            contextScrimVisible: Number(scrimStyle.visibility === 'visible'),
            contextScrimOpacity: Math.round(Number(scrimStyle.opacity) * 1000),
            historyOpen: Number($('history-rail').classList.contains('open')),
            detailsOpen: Number(detailsOpen),
            modelsInert: Number($('model-workspace').inert)
        };
        for (const [key, selector, edge] of [
            ['topbarHeight', '.shell-topbar', 'height'], ['brandTop', '.brand', 'top'],
            ['brandBottom', '.brand', 'bottom'], ['navTop', '.top-navigation', 'top'],
            ['brandLeft', '.brand', 'left'], ['brandRight', '.brand', 'right'],
            ['logoLeft', '#brand-mark', 'left'], ['logoWidth', '#brand-mark', 'width'],
            ['historyCardLeft', '#chats .conversation-row', 'left'],
            ['historySearchLeft', '.history-controls>.search-control', 'left'],
            ['navLeft', '.top-navigation', 'left'], ['navRight', '.top-navigation', 'right'],
            ['navBottom', '.top-navigation', 'bottom'], ['chatHeaderBottom', '.chat-heading', 'bottom'],
            ['modelsHeaderBottom', '.models-content>.page-heading', 'bottom'],
            ['modelsRailHeaderBottom', '.model-sidebar>.rail-heading', 'bottom'],
            ['preparedHeaderBottom', '.prepared-content>.page-heading', 'bottom'],
            ['preparedRailHeaderBottom', '.contextual-rail>.rail-heading', 'bottom'],
            ['historyHeaderBottom', '#history-rail .rail-heading', 'bottom'],
            ['detailsHeaderBottom', '.details-heading', 'bottom'],
            ['detailsWidth', '#details-panel', 'width'],
            ['detailsLeft', '#details-panel', 'left'],
            ['historyLeft', '#history-rail', 'left'],
            ['historyWidth', '#history-rail', 'width'],
            ['chatLeft', '.chat-panel', 'left'], ['chatWidth', '.chat-panel', 'width'],
            ['responseWidth', '.message.assistant', 'width'], ['codeWidth', '.code-block', 'width'],
            ['composerWidth', '#composer', 'width']]) {
            const element = uiElements(selector)[0];
            if (element?.getClientRects().length && !element.closest('[inert]'))
                metrics[key] = Math.round(uiRect(element)[edge] * 100) / 100;
        }
        for (const [key, selector, edge] of [
            ['modelsWorkspaceWidth', '#model-workspace', 'width'],
            ['modelsSurfaceWidth', '.model-view-surface', 'width'],
            ['modelsSearchWidth', '.model-filters>.search-control', 'width'],
            ['topActionsRight', '.shell-top-actions', 'right']]) {
            const element = uiElements(selector)[0];
            if (element?.getClientRects().length)
                metrics[key] = Math.round(uiRect(element)[edge] * 100) / 100;
        }
        for (const id of ['toggle-details','preferences','trash-chat','archive-chat','new','chat-filters-toggle']) {
            const box = $(id).getBoundingClientRect();
            if (box.width) metrics[id + 'CenterX'] = Math.round((box.left + box.width / 2) * 100) / 100;
        }
        const download = $('download-indicator').getBoundingClientRect();
        for (const [name, selector] of [['Icon', '#download-indicator>.icon'],
                                        ['Ring', '#download-indicator>.download-ring']]) {
            const bounds = document.querySelector(selector).getBoundingClientRect();
            metrics['download' + name + 'OffsetX'] = Math.round(
                (bounds.left + bounds.width / 2 - download.left - download.width / 2) * 100) / 100;
            metrics['download' + name + 'OffsetY'] = Math.round(
                (bounds.top + bounds.height / 2 - download.top - download.height / 2) * 100) / 100;
        }
        metrics.transcriptOverflowX = Math.max(0, $('transcript').scrollWidth - $('transcript').clientWidth);
        const measuredSignature = signature + JSON.stringify(metrics);
        if (measuredSignature !== chromeSignature) {
            chromeSignature = measuredSignature;
            send({command : 'chrome', width : innerWidth, height : innerHeight, rects, metrics});
        }
        recordMotionWork('chrome',started);
    });
}
window.overtoneChromeLayout = (height, left, right, fullScreen, transition, active) => {
    // AppKit briefly removes focus during full screen. Let the compositor finish
    // this movement instead of snapping it when that temporary blur arrives.
    chromeTransitioning = Boolean(transition);
    if (typeof active === 'boolean' && window.overtoneWindowActive !== active) {
        window.overtoneWindowActive = active;
        syncWindowMotion();
    }
    const brand = document.querySelector('.shell-brand'), key = [left,fullScreen].join(':'),
          moving = key !== chromeBrandKey, before = moving ? brand.getBoundingClientRect() : null;
    if (moving) {
        chromeBrandAnimation?.cancel();
        chromeBrandAnimation = null;
        chromeBrandKey = key;
    }
    document.documentElement.dataset.chrome = 'mac';
    document.documentElement.dataset.fullscreen = String(Boolean(fullScreen));
    const s = document.documentElement.style;
    s.setProperty('--chrome-height', height + 'px');
    s.setProperty('--mac-controls-left', left + 'px');
    s.setProperty('--mac-controls-right', right + 'px');
    if (moving && document.documentElement.dataset.chromeReady && motionEnabled()) {
        const delta = before.left - brand.getBoundingClientRect().left;
        if (Math.abs(delta) > .5) {
            const animation = brand.animate([{transform:`translateX(${delta}px)`},{transform:'none'}],motionTiming());
            measureMotion('chrome',animation);
            chromeBrandAnimation = animation;
            animation.finished.then(() => {
                if (chromeBrandAnimation === animation) chromeBrandAnimation = null;
                scheduleChromeTargets();
            }, () => {});
        }
    }
    scheduleChromeTargets();
    window.updateChromeAppearance?.();
    if (!document.documentElement.dataset.chromeReady)
        requestAnimationFrame(() => document.documentElement.dataset.chromeReady = 'true');
};
window.overtonePrepareFullScreen = async fullScreen => {
    const style = getComputedStyle(document.documentElement);
    window.overtoneChromeLayout(Number.parseFloat(style.getPropertyValue('--chrome-height')) || 64,
        Number.parseFloat(style.getPropertyValue('--mac-controls-left')) || 112,
        Number.parseFloat(style.getPropertyValue('--mac-controls-right')) || 0, fullScreen, true);
    // AppKit can snapshot WebKit during the native resize; move beforehand, once.
    // Finish before AppKit freezes WebKit for its window snapshot. Native chrome
    // is hidden before this phase and restored only after the brand clears it.
    await chromeBrandAnimation?.finished.catch(() => {});
};
const chromeObserver = new ResizeObserver(entries => {
    // Inspector layout changes do not move the native title-bar hit regions.
    // Send the final measurements once, rather than crossing the bridge each frame.
    if (!detailsTransition || entries.some(entry => entry.target.closest('.shell-topbar')))
        scheduleChromeTargets();
});
for (const element of document.querySelectorAll('.shell-brand,#details-panel,#history-rail'))
    element.addEventListener('transitionend', event => {
        if (event.target === element && ['transform', 'margin-left', 'width'].includes(event.propertyName))
            scheduleChromeTargets();
    });
for (const element of document.querySelectorAll(
         '.shell-topbar,.shell-brand,.top-navigation,.shell-top-actions,.chat-heading,.rail-heading,.details-heading,.page-heading'))
    chromeObserver.observe(element);
navigate('chat');
setHistoryOpen(false);
updateComposer();
