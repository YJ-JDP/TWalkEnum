
from pathlib import Path
from decimal import Decimal, ROUND_HALF_UP
from collections import defaultdict
import csv, hashlib, json, math, re
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch, Rectangle
from matplotlib.legend_handler import HandlerTuple
from matplotlib.ticker import FuncFormatter, NullLocator
from matplotlib.text import Text
import fitz

ROOT=Path(__file__).resolve().parents[1]
SRC=ROOT/'results/paper_tables'
OUT=SRC
FIG=ROOT/'results/figures'
D=Decimal
METHODS=['TWalkEnum','TSPG-style','PathEnum-DFS-style','CPE-startup-style']
LABELS=['TWalkEnum','TSPG','PathEnum','CPE']
GRAPHS=['CM','SD','EU','B09','sMO','SMS','B08','FB','sAU','PR','WT','sSO']
GAPS=['[10%,20%]','[20%,40%]']
LIGHT=['#8DB9D7','#A9D4C5','#F4B47C','#B88ACB']
DARK=['#3570A4','#488364','#C8783E','#6950A3']
HATCH=['//','\\\\','xx','..']
AUDIT=[]
plt.rcParams.update({'font.family':'serif','font.serif':['Times New Roman','Liberation Serif','DejaVu Serif'],
    'mathtext.fontset':'stix','font.size':8,'axes.labelsize':8,'xtick.labelsize':8,'ytick.labelsize':8,
    'legend.fontsize':8,'axes.linewidth':.5,'pdf.fonttype':42,'svg.fonttype':'none',
    'hatch.linewidth':.65,'lines.linewidth':.8,'lines.markersize':3})

def read(name):
    with (SRC/(name+'.tsv')).open(encoding='utf-8-sig',newline='') as f:return list(csv.DictReader(f,delimiter='\t'))

def write(path,rows):
    rows=list(rows);fields=list(dict.fromkeys(k for r in rows for k in r))
    with path.open('w',encoding='utf-8-sig',newline='') as f:
        w=csv.DictWriter(f,fields,delimiter='\t',lineterminator='\n');w.writeheader();w.writerows(rows)

def fmt(x):
    if x in ('',None):return 'NA'
    return str(D(str(x)).quantize(D('.01'),rounding=ROUND_HALF_UP))

def val(row,key):return float(row[key]) if row.get(key) not in ('',None) else np.nan
def mdtable(headers,rows):
    return '\n'.join(['| '+' | '.join(headers)+' |','|'+'---|'*len(headers)]+['| '+' | '.join(map(str,r))+' |' for r in rows])

def plain_log(axis):
    axis.set_yscale('log');axis.yaxis.set_minor_locator(NullLocator())
    axis.yaxis.set_major_formatter(FuncFormatter(lambda x,p:f'{x:g}' if .01<=x<1000 else f'{x:.0e}'.replace('e+0','e').replace('e+','e')))

def style(ax):
    ax.spines[['top','right']].set_visible(False)
    ax.tick_params(length=2,pad=2,width=.5)
    ax.grid(axis='y',color='.87',lw=.35);ax.set_axisbelow(True)

def canvas(height):return plt.figure(figsize=(240/72,height/72))
def axis(fig,y,height=86):return fig.add_axes([36/240,y/(fig.get_figheight()*72),178/240,height/(fig.get_figheight()*72)])
def draw_bars(ax, positions, heights, width, color, hatch=None, hatch_color='black'):
    bars=ax.bar(positions,heights,width,color=color,ec=hatch_color,lw=0,hatch=hatch)
    for bar in bars:
        ax.add_patch(Rectangle(bar.get_xy(),bar.get_width(),bar.get_height(),
                               fill=False,ec='black',lw=.35,zorder=bar.get_zorder()+.01))
    return bars


def bar_key(color, hatch=None, hatch_color='black'):
    return (Patch(fc=color,ec=hatch_color,lw=0,hatch=hatch),
            Patch(fc='none',ec='black',lw=.35))


def legend(fig,handles,labels,y):
    return fig.legend(handles,labels,ncol=len(labels),loc='upper center',bbox_to_anchor=(.5,y),
        frameon=False,handlelength=1,handletextpad=.3,columnspacing=.55,borderaxespad=0,
        handler_map={tuple:HandlerTuple(ndivide=1,pad=0)})

def save(fig,name):
    log_axes=[]
    for ax in fig.axes:
        if ax.get_yscale()=='log':
            low,high=ax.get_ylim()
            lo=math.floor(math.log10(low)+1e-10);hi=math.ceil(math.log10(high)-1e-10)
            ticks=[10.**n for n in range(lo,hi+1)]
            ax.set_ylim(ticks[0],ticks[-1]);ax.set_yticks(ticks)
            ax.yaxis.set_minor_locator(NullLocator())
            ax.yaxis.set_major_formatter(FuncFormatter(lambda x,p:f'{x:g}' if .01<=x<1000 else f'{x:.0e}'.replace('e+0','e').replace('e+','e')))
            assert all(abs(ticks[i+1]/ticks[i]-10)<1e-9 for i in range(len(ticks)-1))
            log_axes.append({'limits':[ticks[0],ticks[-1]],'ticks':ticks,'adjacent_tick_ratio':10})
    fig.canvas.draw();renderer=fig.canvas.get_renderer();issues=[]
    for t in fig.findobj(Text):
        if not t.get_visible() or not t.get_text():continue

        if t.axes is not None:
            pass
        b=t.get_window_extent(renderer)
        if b.width and b.height and (b.x0<-.8 or b.x1>fig.bbox.width+.8 or b.y0<-.8 or b.y1>fig.bbox.height+.8):

            if any(t is tick.label1 and not min(ax.get_ylim())<=tick.get_loc()<=max(ax.get_ylim()) for ax in fig.axes for tick in ax.yaxis.get_major_ticks()):continue
            issues.append(t.get_text())
    fig.savefig(FIG/(name+'.pdf'),metadata={'CreationDate':None,'ModDate':None})
    fig.savefig(FIG/(name+'.svg'),metadata={'Date':None})
    fig.savefig(FIG/(name+'.png'),dpi=300)
    with fitz.open(FIG/(name+'.pdf')) as d:
        sizes=sorted({round(s['size'],3) for b in d[0].get_text('dict')['blocks'] for line in b.get('lines',[]) for s in line['spans']})
        assert all(abs(t.get_fontsize()-8)<.01 for t in fig.findobj(Text) if t.get_visible() and t.get_text()), (name,sizes)
        assert all(abs(x-8)<.01 or (name=='gap_settings' and abs(x-5.6)<.01) for x in sizes),(name,sizes)
        assert not d[0].get_images()
        AUDIT.append(dict(name=name,width_pt=d[0].rect.width,height_pt=d[0].rect.height,font_sizes=sizes,outside_text=issues,log_axes=log_axes))
    plt.close(fig)
    print(name,issues,flush=True)

def plot_gap():
    rows=sorted([r for r in read('gap_timeouts') if r['scope']=='bucket'],key=lambda r:int(r['bucket']))
    lower=[D('0'),D('1.25'),D('2.5'),D('5'),D('10'),D('20'),D('40'),D('80')]
    bounds=[None]+[int(D(100)//pct)+1 for pct in lower[1:]]
    assert bounds[1:]==[81,41,21,11,6,3,2]
    fig=canvas(158);ax=fig.add_axes([32/240,42/158,166/240,91/158])
    draw_bars(ax,range(8),[val(r,'timeout_pct') for r in rows],.62,DARK[2],hatch='xx')
    style(ax)
    ax.set_ylim(0,40);ax.set_yticks([0,10,20,30,40]);ax.set_ylabel('Timeout rate (%)',labelpad=2)
    ax.set_xticks(range(8),['0–1.25','1.25–2.5','2.5–5','5–10','10–20','20–40','40–80','80–100'],rotation=38,ha='right')
    ax.set_xlabel('Gap interval (%)',labelpad=1)
    right=ax.twinx()
    right.plot(range(1,8),bounds[1:],color=DARK[0],marker='s',lw=.8)
    right.set_ylim(0,100);right.set_yticks([0,100])
    right.yaxis.set_minor_locator(NullLocator());right.set_ylabel(r'$H_q$',labelpad=2)
    right.yaxis.set_label_coords(1.04,.5)
    right.tick_params(length=2,pad=2,width=.5)
    right.spines['top'].set_visible(False)
    for x,hops in enumerate(bounds[1:],start=1):
        right.annotate(str(hops),(x,hops),xytext=(0,4),textcoords='offset points',ha='center',va='bottom',fontsize=8,color=DARK[0])
    legend(fig,[bar_key(DARK[2],hatch='xx'),Line2D([],[],color=DARK[0],marker='s')],['Timeout rate',r'$H_q$'],.995)
    write(OUT/'hop_bounds.tsv',[dict(bucket=r['bucket'],delta_low_pct=str(pct),maximum_hops='' if bound is None else bound,bound_status='no_uniform_bound' if bound is None else 'theoretical_upper_bound',timeout_pct=r['timeout_pct']) for r,pct,bound in zip(rows,lower,bounds)])
    save(fig,'gap_settings')

def plot_overall():
    rows=read('query_time');resp=[r for r in read('response_time') if r['scope']=='graph/gap/method']
    lookup={(r['graph'],r['gap'],r['method']):r for r in rows};rl={(r['graph'],r['gap'],r['method']):r for r in resp}
    fig=canvas(287);x=np.arange(12);w=.19
    upper_colors=[tuple(.35*c+.65 for c in matplotlib.colors.to_rgb(color)) for color in LIGHT]
    for j,gap in enumerate(GAPS):
        ax=axis(fig,168 if j==0 else 34,83)
        for k,m in enumerate(METHODS):
            totals=[val(lookup[g,gap,m],'mean_capped_ms') for g in GRAPHS]
            bars=draw_bars(ax,x+(k-1.5)*w,totals,w,upper_colors[k])
            if m!=METHODS[3]:
                response=[val(rl[g,gap,m],'mean_response_ms') for g in GRAPHS]
                assert all(a<=b+1e-9 for a,b in zip(response,totals))
                draw_bars(ax,x+(k-1.5)*w,response,w,DARK[k],hatch=HATCH[k])
        plain_log(ax);style(ax);ax.set_ylim(.01,2e5);ax.set_yticks([.1,10,1000,100000]);ax.set_ylabel('Query time (ms)',labelpad=1)
        ax.set_xticks(x,GRAPHS,rotation=40,ha='right');ax.set_xlim(-.6,11.6)
        fig.text(.52,(135 if j==0 else 2)/287,f'({chr(97+j)}) Δ={gap}',ha='center',va='bottom',fontsize=8)
    legend(fig,[bar_key(upper_colors[k]) for k in range(4)],LABELS,.995)
    fig.text(.5,263/287,'Dark segments: response time',ha='center',va='center')
    save(fig,'query_time')

def plot_result_size():
    rows=[r for r in read('result_size') if not r.get('graph')];lookup={(r['gap'],r['method'],r['bucket']):r for r in rows}
    bins=['1','10','100','1000','10000','100000','1000000','>1M']
    labels=['1','(1,10]','(10,100]','(100,1k]','(1k,10k]','(10k,100k]','(100k,1M]','>1M']
    fig=canvas(279)
    for j,gap in enumerate(GAPS):
        ax=axis(fig,173 if j==0 else 43,83)
        for k,m in enumerate(METHODS):
            y=[val(lookup[gap,m,b],'mean_ms') if (gap,m,b) in lookup else np.nan for b in bins]
            ax.plot(range(8),y,color=LIGHT[k],markeredgecolor=DARK[k],markeredgewidth=.5,marker=['o','s','^','D'][k],ls=['-','--','-.',':'][k])
        plain_log(ax);style(ax);ax.set_ylim(1,1e5);ax.set_yticks([1,100,10000]);ax.set_ylabel('Query time (ms)',labelpad=1)
        ax.set_xticks(range(8),labels,rotation=40,ha='right');ax.set_xlim(-.2,7.2)
        fig.text(.52,(133 if j==0 else 3)/279,f'({chr(97+j)}) Δ={gap}',ha='center',va='bottom')
    legend(fig,[Line2D([],[],color=LIGHT[k],markeredgecolor=DARK[k],marker=['o','s','^','D'][k],ls=['-','--','-.',':'][k]) for k in range(4)],LABELS,.995)
    save(fig,'result_size')

def plot_parallel():
    rows=read('parallel_scaling');fig=canvas(152);ax=axis(fig,28,100);threads=[1,2,4,8,16,32]
    for k,g in enumerate(['EU','WT','sAU','sSO']):
        tab={int(r['workers']):val(r,'speedup') for r in rows if r['graph']==g}
        ax.plot(threads,[tab[t] for t in threads],color=LIGHT[k],markeredgecolor=DARK[k],markeredgewidth=.5,marker=['o','s','^','D'][k],ls=['-','--','-.',':'][k],label=g)
    ax.set_xscale('log',base=2);ax.set_xticks(threads,[str(x) for x in threads]);ax.set_xlim(.9,36);ax.set_ylim(0,11);ax.set_yticks([0,2,4,6,8,10]);style(ax)
    ax.set_xlabel('Number of threads',labelpad=1);ax.set_ylabel('Speedup',labelpad=1)
    handles,labels=ax.get_legend_handles_labels();legend(fig,handles,labels,.995)
    save(fig,'parallel_scaling')

def plot_indexed():
    with matplotlib.rc_context({'hatch.linewidth':.3}):
        rows=read('indexed_query_time');methods=['PoolIndex+TWalkEnum','TWalkEnum','Indexed-TSPG-style','Indexed-PathEnum-DFS-style','Indexed-CPE-startup-style']
        labels=['PoolIndex','TWalkEnum','TSPG','PathEnum','CPE'];colors=[DARK[0],*LIGHT];outlines=[DARK[0],*DARK];hatches=['//',*HATCH]
        lookup={(r['dataset'],r['method']):val(r,'mean_capped_ms') for r in rows};fig=canvas(165);ax=axis(fig,38,101);x=np.arange(12);w=.15
        for k,m in enumerate(methods):draw_bars(ax,x+(k-2)*w,[lookup[g,m] for g in GRAPHS],w,colors[k],hatch=hatches[k],hatch_color=outlines[k])
        plain_log(ax);style(ax);ax.set_ylim(.01,8e4);ax.set_yticks([.1,10,1000]);ax.set_ylabel('Query time (ms)',labelpad=1);ax.set_xticks(x,GRAPHS,rotation=40,ha='right');ax.set_xlim(-.6,11.6)
        legend(fig,[bar_key(colors[k],hatch=hatches[k],hatch_color=outlines[k]) for k in range(5)],labels,.995)
        save(fig,'indexed_query_time')

def plot_memory():
    with matplotlib.rc_context({'hatch.linewidth':.3}):
        rows=read('memory_usage')
        lookup={(r['graph'],r['method']):r for r in rows}
        graphs=['EU','WT','sAU','sSO']
        fig=canvas(154);ax=axis(fig,33,99);x=np.arange(4);w=.2
        for k,m in enumerate(METHODS):
            draw_bars(ax,x+(k-1.5)*w,[val(lookup[g,m],'mean_MiB') for g in graphs],w,LIGHT[k],hatch=HATCH[k],hatch_color=DARK[k])
        plain_log(ax);style(ax);ax.set_ylabel('Extra space (MiB)',labelpad=1);ax.set_xticks(x,graphs);ax.set_ylim(.1,10000)
        fig.text(.52,4/154,'Δ=[20%,40%]',ha='center',va='bottom')
        legend(fig,[bar_key(LIGHT[k],hatch=HATCH[k],hatch_color=DARK[k]) for k in range(4)],LABELS,.995)
        save(fig,'memory_usage')

if __name__=='__main__':
    FIG.mkdir(parents=True,exist_ok=True)
    for function in [plot_gap,plot_overall,plot_result_size,plot_memory,plot_parallel,plot_indexed]:
        function()
    assert all(not row['outside_text'] for row in AUDIT),AUDIT
    (OUT/'figure_validation.json').write_text(json.dumps(AUDIT,indent=2),encoding='utf-8')
