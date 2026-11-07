'use strict';
// A tour teaches actions on the real controls. Navigation and dialogs are never simulated.
const featureGuide = {steps:[],index:0,anchor:null,targets:[],effects:[],busy:false,epoch:0,locks:new Set(),focus:null};
const guideHighlight = make('div','','guide-highlight');
guideHighlight.id = 'guide-highlight';guideHighlight.setAttribute('popover','manual');
guideHighlight.dataset.motionOwned='true';$('feature-guide').dataset.motionOwned='true';
guideHighlight.setAttribute('aria-hidden','true');
if (typeof guideHighlight.showPopover !== 'function') guideHighlight.hidden = true;
document.body.append(guideHighlight);
function guideShape(tag,attributes={}) {
    const node=document.createElementNS('http://www.w3.org/2000/svg',tag);
    for (const [key,value] of Object.entries(attributes)) node.setAttribute(key,value);
    return node;
}
const guideSVG=guideShape('svg',{width:'100%',height:'100%','aria-hidden':'true'}),
      guideMask=guideShape('mask',{id:'feature-guide-mask',maskUnits:'userSpaceOnUse'}),
      guideHoles=guideShape('g'),guideOutlines=guideShape('g',{'class':'guide-outlines'}),guideCue=guideShape('g',{'data-passive-motion':'',opacity:0}),defs=guideShape('defs');
guideMask.append(guideShape('rect',{width:'100%',height:'100%',fill:'white'}),guideHoles);defs.append(guideMask);
guideSVG.append(defs,guideShape('rect',{width:'100%',height:'100%',fill:'black',opacity:'.22',mask:'url(#feature-guide-mask)'}),guideOutlines,guideCue);
guideHighlight.append(guideSVG);

const guideQuery = selector => [...document.querySelectorAll(selector || ':not(*)')]
    .filter(el => el.isConnected && !el.closest('[hidden],.inactive-view') && !!el.getClientRects().length);
function unlockGuide() {
    for (const node of featureGuide.targets) {
        const ids=(node.getAttribute('aria-describedby') || '').split(' ').filter(id => id && id!=='guide-description');
        if (ids.length) node.setAttribute('aria-describedby',ids.join(' '));else node.removeAttribute('aria-describedby');
    }
    for (const node of featureGuide.locks) {node.inert=false;node.removeAttribute('data-guide-blocked');}
    featureGuide.locks.clear();
    document.querySelectorAll('[data-guide-readonly]').forEach(node => node.removeAttribute('data-guide-readonly'));
}
function lockGuide() {
    unlockGuide();
    if (!featureGuide.steps.length) return;
    const step=featureGuide.steps[featureGuide.index], actions=featureGuide.busy ? [] : guideQuery(step?.action),
          targets=featureGuide.busy ? [] : guideQuery(step?.target),
          keep=[$('feature-guide'),guideHighlight,...actions];
    featureGuide.targets=actions;
    for (const node of actions) node.setAttribute('aria-describedby',
        [...new Set((node.getAttribute('aria-describedby') || '').split(' ').filter(Boolean).concat('guide-description'))].join(' '));
    for (const target of targets) if (!actions.some(action => action===target || target.contains(action)))
        target.setAttribute('data-guide-readonly','');
    function visit(parent) {
        for (const child of parent.children) {
            if (child.inert || ['SCRIPT','STYLE','LINK'].includes(child.tagName)) continue;
            if (keep.some(node => node===child)) continue;
            if (keep.some(node => child.contains(node))) visit(child);
            else {child.inert=true;child.setAttribute('data-guide-blocked','');featureGuide.locks.add(child);}
        }
    }
    visit(document.body);hideTip();window.updateAppMenu?.();
}
async function settleGuideMotion() {
    for (let pass=0;pass<3;pass++) {
        await new Promise(resolve => requestAnimationFrame(resolve));
        const effects=uiAnimations().filter(a => Number.isFinite(a.effect?.getComputedTiming().endTime) && a.playState !== 'finished');
        if (!effects.length) return;
        await Promise.allSettled(effects.map(a => a.finished));
    }
}
async function hideGuideSurfaces() {
    guideAnchorObserver.disconnect();featureGuide.anchor=null;
    featureGuide.pulse?.cancel();featureGuide.pulse=null;guideCue.replaceChildren();
    const surfaces=[$('feature-guide'),guideHighlight].filter(surfaceIsOpen),
          opacity=surfaces.map(el => Number(getComputedStyle(el).opacity));
    featureGuide.effects.forEach(effect => effect.cancel());featureGuide.effects=[];
    if (motionEnabled() && surfaces.length) {
        const effects=featureGuide.effects=surfaces.map((el,i) => el.animate([{opacity:opacity[i]},{opacity:0}],motionTiming('fast')));
        await Promise.allSettled(effects.map(effect => effect.finished));
        if (featureGuide.effects !== effects) return;
        featureGuide.effects=[];effects.forEach(effect => effect.cancel());
    }
    surfaces.forEach(surfaceClose);
}
async function endFeatureGuide(remember=true) {
    if (!featureGuide.steps.length) return;
    featureGuide.epoch++;featureGuide.busy=true;featureGuide.steps=[];
    cancelAnimationFrame(guidePlacement);guidePlacement=0;
    await hideGuideSurfaces();unlockGuide();
    if (remember && current) send({command:'preferences',value:{...current.preferences,guideSeen:7}});
    document.body.append(guideHighlight,$('feature-guide'));
    featureGuide.busy=false;
    const focus=featureGuide.focus;
    if (keyboardInput && focus?.isConnected && !hiddenUIElement(focus)) focus.focus({preventScroll:true});
    controls();
}
let guidePlacement=0;
const guideAnchorObserver=new ResizeObserver(queueGuidePlacement);
function queueGuidePlacement() {
    if (!featureGuide.anchor || featureGuide.busy || guidePlacement) return;
    guidePlacement=requestAnimationFrame(() => {
        guidePlacement=0;placeFeatureGuide();
        if (uiAnimations().some(a => a.playState === 'running' && Number.isFinite(a.effect?.getComputedTiming().endTime))) queueGuidePlacement();
    });
}
function placeFeatureGuide() {
    const step=featureGuide.steps[featureGuide.index], anchor=guideQuery(step?.target)[0];
    if (!anchor || featureGuide.busy) return;
    featureGuide.anchor=anchor;
    const r=uiRect(anchor), regions=[anchor,...featureGuide.targets.filter(node => node!==anchor && !anchor.contains(node))];
    guideHoles.replaceChildren();guideOutlines.replaceChildren();guideCue.replaceChildren();
    for (const node of regions) {
        const box=uiRect(node), inset=4, left=box.left-inset,top=box.top-inset,
              style=getComputedStyle(node), radius=Math.min(box.width/2,box.height/2,
                parseFloat(style.borderRadius)||parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--r-surface'))||0),
              attributes={x:left,y:top,width:box.width+inset*2,
                height:box.height+inset*2,rx:radius+inset};
        guideHoles.append(guideShape('rect',{...attributes,fill:'black'}));
        guideOutlines.append(guideShape('rect',{...attributes,fill:'none',stroke:'var(--accent)','stroke-width':2}));
        if (featureGuide.targets.includes(node)) guideCue.append(guideShape('rect',{...attributes,x:left-3,y:top-3,width:box.width+inset*2+6,height:box.height+inset*2+6,rx:radius+inset+3,fill:'none',stroke:'var(--accent)','stroke-width':5}));
    }
    const surface=$('feature-guide');surface.style.maxHeight='';
    const w=surface.offsetWidth,h=surface.offsetHeight,
          horizontal=[r.right+16,r.left-w-16].find(x => x>=12 && x+w<=innerWidth-12),
          vertical=[r.bottom+16,r.top-h-16].find(y => y>=12 && y+h<=innerHeight-12);
    const x=horizontal ?? Math.max(12,Math.min(innerWidth-w-12,r.left+(r.width-w)/2)),
          y=horizontal !== undefined ? Math.max(12,Math.min(innerHeight-h-12,r.top)) :
              vertical ?? Math.max(12,Math.min(innerHeight-h-12,r.top+16));
    surface.style.left=x+'px';surface.style.top=y+'px';surface.style.maxHeight=Math.max(0,innerHeight-y-12)+'px';
}
async function featureGuideStep() {
    const step=featureGuide.steps[featureGuide.index];
    if (!step) {endFeatureGuide();return;}
    const epoch=++featureGuide.epoch;
    featureGuide.busy=true;lockGuide();await hideGuideSurfaces();
    if (epoch !== featureGuide.epoch || !featureGuide.steps.length) return;
    await settleGuideMotion();
    if (epoch !== featureGuide.epoch || !featureGuide.steps.length) return;
    unlockGuide();
    const anchor=guideQuery(step.target)[0];
    if (!anchor) {endFeatureGuide(false);return;}
    anchor.scrollIntoView({block:'nearest',inline:'nearest',behavior:'instant'});
    const host=anchor.closest('dialog[open]') || document.body;
    host.append(guideHighlight,$('feature-guide'));
    featureGuide.anchor=anchor;guideAnchorObserver.observe(anchor);
    $('guide-heading').textContent=tr(step.title);
    const description=$('guide-description'),parts=tr(step.text).split('{mark}');
    description.replaceChildren();
    parts.forEach((part,index) => {
        if (index) {const mark=verifiedModelMark();mark.removeAttribute('data-tip');description.append(mark);}
        description.append(document.createTextNode(part));
    });
    $('guide-position').textContent=(featureGuide.index+1)+' / '+featureGuide.steps.length;
    $('previous-guide').hidden=true;
    $('next-guide').hidden=!!step.action;
    $('next-guide').textContent=tr(featureGuide.index===featureGuide.steps.length-1 ? 'Fine' : 'Avanti');
    featureGuide.busy=false;lockGuide();
    surfaceOpen(guideHighlight);surfaceOpen($('feature-guide'));placeFeatureGuide();
    startGuideCue();
    if (motionEnabled()) featureGuide.effects=[$('feature-guide'),guideHighlight].map(el => el.animate([{opacity:0},{opacity:1}],motionTiming('fast')));
    const control=step.action ? featureGuide.targets[0] : $('next-guide');
    if (keyboardInput) control?.focus({preventScroll:true});
}
const tourRead=(target,title,text) => ({target,title,text}),
      tourAction=(target,action,title,text,complete) => ({target,action,title,text,complete});
function parameterTour() {
    return [activeLibraryModel() ? tourRead('.model-param-presets','Parametri per ogni modello','Consigliato adatta i parametri a questo modello e al computer. Le modifiche salvate rimangono associate soltanto a questo modello.') :
        tourRead('.model-parameter-fields','Parametri del modello','Qui regoli input, output e generazione. I parametri disponibili dipendono dalle capacità comunicate dal server.'),
        ...(activeLibraryModel() ? [tourRead('#model-memory','Memoria e contesto','La stima si aggiorna mentre cambi i parametri. Contesto e output determinano quanto il modello può leggere e generare.')] : []),
        tourAction('#apply-model-parameters','#apply-model-parameters','Salva i parametri','Premi Salva per applicare i parametri e tornare alla chat.',() => !$('model-parameters').open)];
}
window.overtoneGuide=(scope='') => {
    if (!current || featureGuide.steps.length || featureGuide.busy) return;
    const dialog=[...document.querySelectorAll('dialog[open]')].at(-1);
    if (dialog && !['model-parameters','model-advice'].includes(dialog.id)) return;
    featureGuide.focus=activeUIElement();closeFloatingSurfaces();
    const steps=[];
    if (dialog?.id==='model-parameters') steps.push(...parameterTour());
    else if (dialog?.id==='model-advice') steps.push(tourRead('#model-advice .configuration-content','Consigli per il tuo utilizzo','I suggerimenti confrontano risorse, utilizzo e varianti. Il contrassegno {mark} identifica i modelli verificati da OverTone.'));
    else {
        if (route!=='chat') steps.push(tourAction('#nav-chat','#nav-chat','Apri Chat','Premi Chat per iniziare la guida.',() => route==='chat'));
        if (libraryState?.models.length || current.connection.model) steps.push(tourAction('#model-menu','#model-menu','Scegli modello','Premi la freccia per vedere i modelli installati.',() => surfaceIsOpen($('model-popover'))),
            tourAction('#model-popover','#model-menu','I modelli installati','Qui scegli il modello da usare. Premi di nuovo la freccia per chiudere la lista.',() => !surfaceIsOpen($('model-popover'))));
        if (current.connection.model) steps.push(tourAction('#configure','#configure','Parametri del modello','Premi il nome del modello per aprire i suoi parametri.',() => $('model-parameters').open),...parameterTour());
        if (!detailsOpen) steps.push(tourAction('#toggle-details','#toggle-details','Dettagli','Apri Dettagli con questo pulsante.',() => detailsOpen));
        steps.push(
            tourAction('#details-panel','#toggle-details','Informazioni nel contesto','Dettagli raccoglie contesto, risorse e informazioni tecniche della schermata aperta. Premi di nuovo il pulsante per chiuderlo.',() => !detailsOpen),
            tourAction('#nav-models','#nav-models','Apri Modelli','Premi Modelli per vedere la libreria e trovare nuovi modelli.',() => route==='models'));
        if (modelTab!=='installed') steps.push(tourAction('#tab-installed,[data-model-tab=installed]','#tab-installed,[data-model-tab=installed]','La tua libreria','Premi Installati per vedere i modelli presenti sul computer.',() => modelTab==='installed'));
        steps.push(tourRead('#models-installed','La tua libreria','Qui trovi i modelli installati. Il contrassegno {mark} identifica le varianti verificate da OverTone.'),
            tourAction('#tab-search,[data-model-tab=search]','#tab-search,[data-model-tab=search]','Trova un modello','Premi Trova per aprire i cataloghi.',() => modelTab==='search'),
            tourRead('#models-search','Scegli dove cercare','I cataloghi offrono varianti diverse. Ogni modello viene associato a un motore compatibile.'),
            tourAction('#show-advice','#show-advice','Consigliati per te','Apri i suggerimenti adattati al tuo utilizzo e al computer.',() => $('model-advice').open),
            tourRead('#model-advice .configuration-content','Consigli per il tuo utilizzo','I suggerimenti confrontano risorse, utilizzo e varianti. Il contrassegno {mark} identifica i modelli verificati da OverTone.'));
    }
    featureGuide.steps=steps;featureGuide.index=0;featureGuideStep();
};
$('next-guide').onclick=() => {if (!featureGuide.busy && !featureGuide.steps[featureGuide.index]?.action) {featureGuide.index++;featureGuideStep();}};
$('skip-guide').onclick=() => endFeatureGuide();
function guideEvent(event) {
    if (!featureGuide.steps.length) return;
    if (event.type==='keydown' && event.key==='Escape') {
        event.preventDefault();event.stopImmediatePropagation();endFeatureGuide();return;
    }
    const target=event.target, guide=$('feature-guide'), step=featureGuide.steps[featureGuide.index],
          action=!featureGuide.busy && featureGuide.targets.some(node => node===target || node.contains(target));
    if (guide.contains(target)) return;
    if (!action) {
        event.preventDefault();event.stopImmediatePropagation();return;
    }
    if (event.type==='click') {
        stopGuideCue();
        const epoch=featureGuide.epoch;featureGuide.busy=true;
        queueMicrotask(async () => {
            await settleGuideMotion();
            if (epoch!==featureGuide.epoch || !featureGuide.steps.length) return;
            featureGuide.busy=false;
            if (step.complete?.()) {featureGuide.index++;featureGuideStep();}
            else {lockGuide();startGuideCue();}
        });
    }
}
for (const type of ['pointerdown','pointerup','click','dblclick','contextmenu','selectstart','keydown','wheel'])
    onUIEvent(type,guideEvent,{capture:true,passive:false});
window.guideParametersSaved=() => {
    const step=featureGuide.steps[featureGuide.index], epoch=featureGuide.epoch;
    if (step?.action !== '#apply-model-parameters') return false;
    featureGuide.busy=true;lockGuide();closeDialog($('model-parameters'));
    queueMicrotask(async () => {
        await settleGuideMotion();
        if (epoch!==featureGuide.epoch || !featureGuide.steps.length) return;
        featureGuide.index++;featureGuideStep();
    });
    return true;
};
function startGuideCue() {
    stopGuideCue();
    if (!motionEnabled() || featureGuide.busy || !featureGuide.steps[featureGuide.index]?.action || !guideCue.children.length) return;
    const box=guideCue.getBBox();
    if (!box.width || !box.height) return;
    const spread=Math.max(0,Math.min(6,box.x-3,box.y-3,innerWidth-box.x-box.width-3,innerHeight-box.y-box.height-3)),
          scale='scale('+((box.width+2*spread)/box.width)+','+((box.height+2*spread)/box.height)+')';
    guideCue.style.transformBox='fill-box';guideCue.style.transformOrigin='center';
    featureGuide.pulse=guideCue.animate([{opacity:0,transform:'scale(1)',offset:0},
        {opacity:1,transform:'scale(1)',offset:.18},{opacity:0,transform:scale,offset:1}],
        {duration:1450,iterations:Infinity,easing:'ease-out'});
}
function stopGuideCue() {featureGuide.pulse?.cancel();featureGuide.pulse=null;}
document.addEventListener('visibilitychange',() => {if (document.hidden) stopGuideCue();else startGuideCue();});
motionPreference.addEventListener('change',startGuideCue);
window.queueGuidePlacement=queueGuidePlacement;
window.addEventListener('resize',() => {if (featureGuide.steps.length) lockGuide();queueGuidePlacement();});
window.addEventListener('focus',() => {queueGuidePlacement();startGuideCue();});
document.addEventListener('visibilitychange',queueGuidePlacement);
document.addEventListener('scroll',queueGuidePlacement,{capture:true,passive:true});
window.guideAllowsClosing=() => !featureGuide.steps.length || !featureGuide.busy && featureGuide.targets.some(node => node.id==='close-model-parameters' || node.id==='close-advice');
window.guideIsActive=() => featureGuide.steps.length > 0 || featureGuide.busy;
window.guideSnapshot=() => {
    const el=$('feature-guide');
    if (!surfaceIsOpen(el)) return {open:false,preparing:featureGuide.busy};
    const r=el.getBoundingClientRect(),anchor=featureGuide.anchor?.getBoundingClientRect(),h=guideOutlines.firstElementChild?.getBoundingClientRect() || guideHighlight.getBoundingClientRect();
    return {open:true,preparing:featureGuide.busy,step:featureGuide.index,total:featureGuide.steps.length,anchor:featureGuide.anchor?.id || featureGuide.anchor?.className,
        context:featureGuide.steps[featureGuide.index]?.title,action:featureGuide.steps[featureGuide.index]?.action || '',blocked:featureGuide.locks.size,host:el.parentElement.id || 'body',
        x:r.x,y:r.y,width:r.width,height:r.height,anchorBottom:anchor?.bottom,
        outside:r.left<0 || r.right>innerWidth || r.top<0 || r.bottom>innerHeight,
        verificationMarks:[...$('guide-description').querySelectorAll('.compatibility-mark')].map(mark => ({glyph:mark.querySelector('use')?.getAttribute('href'),width:mark.getBoundingClientRect().width,height:mark.getBoundingClientRect().height})),
        overflow:el.scrollWidth>el.clientWidth+1,pulse:featureGuide.pulse?.playState || 'none',cueOpacity:Number(getComputedStyle(guideCue).opacity),cueMs:featureGuide.pulse?.currentTime || 0,
        highlight:{visible:surfaceIsOpen(guideHighlight),regions:guideOutlines.children.length,shade:'rgba(0,0,0,.22)',left:h.left,top:h.top,width:h.width,height:h.height,
            centerX:anchor ? (h.left+h.right-anchor.left-anchor.right)/2 : null,
            centerY:anchor ? (h.top+h.bottom-anchor.top-anchor.bottom)/2 : null,
            contains:anchor && h.left<=anchor.left+.5 && h.right>=anchor.right-.5 && h.top<=anchor.top+.5 && h.bottom>=anchor.bottom-.5}};
};

window.activeGuideSurfaces=() => [guideHighlight,$('feature-guide'),...(featureGuide.steps.length && featureGuide.anchor?.closest('[popover]') ? [featureGuide.anchor.closest('[popover]')] : [])];
