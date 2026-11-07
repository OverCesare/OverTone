'use strict';
// A facet owns its value in a hidden native select. Lists, chips and the anchored
// panel project that same state; changing a view does not re-create its filters.
const filterViews = new Map();
const filterSelection = source => source.multiple ? [...source.selectedOptions].map(option => option.value)
    : source.value==='all' ? [] : [source.value];
function filterOptions(id, values, count = null) {
    const source=$(id), selected=filterSelection(source),
          choices=values.map(([value,label]) => [value,label,count?.(value)]),
          signature=JSON.stringify([language,choices]);
    if (source.dataset.options===signature) return;
    source.dataset.options=signature;
    source.replaceChildren(...choices.map(([value,label,total]) => {
        const option=new Option(tr(label),value,false,selected.includes(value) || !source.multiple && !selected.length && value==='all');
        option.dataset.label=label;
        if (total!==undefined) option.dataset.count=String(total);
        return option;
    }));
    source.dispatchEvent(new Event('optionschange'));
}
function filterPanel(prefix, definitions, changed, {query=prefix+'-query',host=null}={}) {
    const field=query ? $(query) : null, toolbar=field?.parentElement || host,
          panel=make('div','','library-filters'), chips=make('div','','filter-chips model-filter-chips'), sources=[];
    let active=0, totals=null, queued=false, opening=false, signature='', chipSignature='';
    for (const definition of definitions) {
        let source=$(prefix+'-'+definition.id);
        if (!source) {source=make('select');source.id=prefix+'-'+definition.id;toolbar.append(source);}
        source.hidden=true;source.multiple=!!definition.multiple;
        source.dataset.catalogLabel=definition.label;source.dataset.filterIcon=definition.icon;
        if (definition.bound) source.dataset.filterBound=definition.bound;
        source.toggleAttribute('data-filter-order',!!definition.order);
        localAccessible(source,definition.label);sources.push(source);
        filterOptions(source.id,definition.values);
        source.addEventListener('change',changed);
        source.addEventListener('change',update);
        source.addEventListener('optionschange',update);
    }
    const reset = () => {
        for (const source of sources.filter(source => !source.hasAttribute('data-filter-order'))) {
            if (source.multiple) for (const option of source.options) option.selected=false;
            else source.value='all';
        }
        sources[0].dispatchEvent(new Event('change'));
    };
    const title=make('div','','filter-panel-heading row'), body=make('div','','filter-panel-body'),
          nav=make('nav','','filter-categories'), values=make('div','','filter-values'),
          resetButton=button('Azzera filtri',reset), count=make('small','','muted');
    title.append(localLabel('strong','Filtri'),count,make('span','','grow'),resetButton);
    count.setAttribute('role','status');resetButton.id=prefix+'-filters-reset';
    localAccessible(nav,'Filtri');nav.setAttribute('role','tablist');nav.setAttribute('aria-orientation','vertical');
    body.append(nav,values);panel.append(title,body);panel.id=prefix+'-filters';
    panel.setAttribute('popover','auto');panel.setAttribute('role','dialog');localAccessible(panel,'Filtri');
    if (typeof panel.showPopover!=='function') panel.hidden=true;
    document.body.append(panel);
    const toggle=actionButton('Filtri','list-filter',event => {
        event.preventDefault();hideTip();
        if (surfaceIsOpen(panel) || opening) {opening=false;surfaceClose(panel);return;}
        opening=true;openWhenStable();
    });
    toggle.id=prefix+'-filters-toggle';toggle.setAttribute('aria-haspopup','dialog');toggle.setAttribute('aria-expanded','false');
    toggle.setAttribute('aria-controls',panel.id);toggle.setAttribute('popovertarget',panel.id);
    if (field) {
        const search=make('div','','search-control flow-surface'), submit=toolbar.querySelector('button[type=submit],button:not([type])');
        toolbar.prepend(search);
        if (submit) search.append(submit,field,toggle);else search.append(icon('search'),field,toggle);
        toolbar.after(chips);
    } else {toolbar.append(toggle);toolbar.after(chips);}
    chips.hidden=true;
    function openWhenStable() {
        if (!opening) return;
        if (typeof afterDetailsMotion==='function' && afterDetailsMotion(toggle,openWhenStable)) return;
        opening=false;
        if (hiddenUIElement(toggle)) return;
        signature='';render();surfaceOpen(panel);positionSurface(panel,toggle,true);
        nav.scrollTop=values.scrollTop=0;
    }
    function render() {
        const next=JSON.stringify([language,active,totals,sources.map(source => [filterSelection(source),
            [...source.options].map(option => [option.value,option.textContent,option.dataset.count])])]);
        if (signature===next) return;signature=next;
        const focus=document.activeElement, focusID=focus?.id, name=focus?.name, value=focus?.value,
              top=values.scrollTop, navTop=nav.scrollTop;
        nav.replaceChildren();values.replaceChildren();
        count.textContent=totals ? countFormat.format(totals.shown)+' / '+countFormat.format(totals.total) : '';
        resetButton.firstChild.textContent=tr('Azzera filtri');
        for (const [index,source] of sources.entries()) {
            const chosen=filterSelection(source), category=button(source.getAttribute('aria-label'),() => {
                active=index;render();values.scrollTop=0;
                if (keyboardInput) nav.children[index].focus({preventScroll:true});
                enterSurface(values);
            },true);
            category.id=source.id+'-category';category.dataset.filterSource=source.id;
            category.prepend(icon(source.dataset.filterIcon));
            category.setAttribute('role','tab');category.setAttribute('aria-selected',String(active===index));
            category.setAttribute('aria-controls',prefix+'-filter-values');category.tabIndex=active===index ? 0 : -1;
            if (chosen.length) category.append(make('small',countFormat.format(chosen.length),'filter-category-count'));
            nav.append(category);
            if (active!==index) continue;
            const group=make('fieldset');group.id=prefix+'-filter-values';group.setAttribute('role','tabpanel');
            group.setAttribute('aria-labelledby',category.id);group.append(make('legend',source.getAttribute('aria-label')));
            for (const option of source.options) {
                const label=make('label','','filter-choice'), input=make('input');
                input.type=source.multiple ? 'checkbox' : 'radio';input.name=source.id;input.value=option.value;
                input.checked=source.multiple ? option.selected : source.value===option.value;
                input.disabled=option.dataset.count==='0' && !input.checked && option.value!=='all';
                input.onchange=() => {
                    if (source.multiple) option.selected=input.checked;else source.value=option.value;
                    source.dispatchEvent(new Event('change'));
                };
                const caption=make('span',option.textContent);
                if (option.value==='verified') {const mark=verifiedModelMark();mark.removeAttribute('data-tip');mark.setAttribute('aria-hidden','true');caption.prepend(mark);}
                label.classList.toggle('selected',input.checked);label.append(input,caption);
                if (option.dataset.count!==undefined) label.append(make('small',countFormat.format(Number(option.dataset.count)),'filter-count'));
                group.append(label);
            }
            values.append(group);
        }
        resetButton.disabled=!sources.some(source => !source.hasAttribute('data-filter-order') && filterSelection(source).length);
        if (keyboardInput) {
            const restore=name ? values.querySelector('input[name="'+CSS.escape(name)+'"][value="'+CSS.escape(value)+'"]') : focusID ? panel.querySelector('#'+CSS.escape(focusID)) : null;
            restore?.focus({preventScroll:true});
        }
        values.scrollTop=top;nav.scrollTop=navTop;positionSurface(panel,toggle,true);
    }
    nav.onkeydown=event => {
        if (!['ArrowDown','ArrowUp','Home','End'].includes(event.key)) return;
        event.preventDefault();active=event.key==='Home' ? 0 : event.key==='End' ? sources.length-1
            : (active+(event.key==='ArrowDown' ? 1 : -1)+sources.length)%sources.length;
        render();nav.children[active].focus({preventScroll:true});
    };
    function update() {
        if (typeof afterDetailsMotion==='function' && afterDetailsMotion(chips,update)) return;
        const applied=sources.flatMap(source =>
            [...source.selectedOptions].filter(option => option.value!=='all').map(option => ({source,option}))),
              next=JSON.stringify([language,applied.map(({source,option}) => [source.id,option.value,option.textContent])]);
        const filtered=applied.filter(({source}) => !source.hasAttribute('data-filter-order')).length;
        toggle.setAttribute('aria-pressed',String(filtered>0));toggle.dataset.count=filtered || '';
        if (chipSignature!==next) {
            chipSignature=next;
            chips.replaceChildren(...applied.map(({source,option}) => {
                const caption=source.hasAttribute('data-filter-order') || option.value==='unknown'
                    ? source.getAttribute('aria-label')+': '+option.textContent
                    : (source.dataset.filterBound ? source.dataset.filterBound==='max' ? '≤ ' : '≥ ' : '')+option.textContent;
                const chip=button(caption,() => {
                    if (source.multiple) option.selected=false;else source.value='all';
                    source.dispatchEvent(new Event('change'));
                },true);
                chip.className='filter-chip';chip.dataset.filterSource=source.id;chip.dataset.filterValue=option.value;
                localAccessible(chip,'Rimuovi filtro');chip.setAttribute('aria-label',tr('Rimuovi filtro')+': '+source.getAttribute('aria-label')+': '+option.textContent);
                chip.append(icon('x'));return chip;
            }));
            if (typeof revealStack==='function') revealStack(chips,applied.length>0);else chips.hidden=!applied.length;
        }
        if (surfaceIsOpen(panel) && !queued) {
            queued=true;queueMicrotask(() => {queued=false;if (surfaceIsOpen(panel)) render();});
        }
    }
    panel.addEventListener('toggle',() => toggle.setAttribute('aria-expanded',String(surfaceIsOpen(panel))));
    const api={clear:reset,sources,summary(shown,total){totals={shown,total};update();},
        snapshot(){const rect=panel.getBoundingClientRect();return {open:surfaceIsOpen(panel),active:sources[active].id,totals,
            x:rect.x,y:rect.y,width:rect.width,height:rect.height,viewportWidth:innerWidth,viewportHeight:innerHeight,
            overflow:panel.scrollWidth>panel.clientWidth+1 || values.scrollWidth>values.clientWidth+1,
            navOverflow:nav.scrollWidth>nav.clientWidth+1,
            choices:[...values.querySelectorAll('.filter-choice')].map(label => ({value:label.querySelector('input').value,
                width:label.getBoundingClientRect().width,height:label.getBoundingClientRect().height,overflow:label.scrollWidth>label.clientWidth+1})),
            categories:sources.map(source => ({id:source.id,selected:filterSelection(source),options:[...source.options].map(option =>
                ({value:option.value,count:option.dataset.count===undefined ? undefined : Number(option.dataset.count)}))})),
            chips:appliedChipCount(),activeFilters:Number(toggle.dataset.count)||0,workMs:api.workMs || 0};}};
    function appliedChipCount(){return chips.childElementCount;}
    filterViews.set(prefix,api);update();return api;
}
