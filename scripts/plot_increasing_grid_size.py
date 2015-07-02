#!/usr/bin/env python

hw_ctr_labels = {
                    '':(),
                    'TLB':[('L1 DTLB miss rate sum', 'tlb_', 'tlb')],
                    'DATA':[('Load to Store ratio avg', 'cpu_', 'data')],
                    'L2':[('Bytes/LUP', 'L2_', 'l2 vol')],
                    'L3':[('Bytes/LUP', 'L3_', 'l3 vol')],
                    'MEM':[('GB/s', 'bw_', 'mem bw'), ('Bytes/LUP', 'MEM_vol_', 'mem vol')],
                    'ENERGY':[('pJ/LUP', 'energy_', 'total energy')] }
 
def main():
  import sys
  from scripts.utils import get_stencil_num, load_csv
  raw_data = load_csv(sys.argv[1])


  req_fields = [('MStencil/s  MAX', float), ('Precision', int), ('Global NX', int), ('Number of time steps', int), ('Number of tests', int)]

  hw_ctr_fields = {
                    '':[],
                    'TLB':[('L1 DTLB miss rate sum', float)],
                    'DATA':[('Load to Store ratio avg', float)],
                    'L2':[('L2 data volume sum', float)],
                    'L3':[('L3 data volume sum', float)],
                    'MEM':[('Total Memory Transfer', float),('Sustained Memory BW', float)],
                    'ENERGY':[('Energy', float), ('Energy DRAM', float), ('Power',float), ('Power DRAM', float)]}

 
  duplicates = set()
  plots = dict()
  for k in raw_data:

    # Use single field to represent the performance
    if 'Total RANK0 MStencil/s MAX' in k.keys():
      if(k['Total RANK0 MStencil/s MAX']!=''):
        k['MStencil/s  MAX'] = k['MWD main-loop RANK0 MStencil/s MAX'] 
    # temporary for deprecated format
    if 'RANK0 MStencil/s  MAX' in k.keys():
      if k['RANK0 MStencil/s  MAX']!='':
        k['MStencil/s  MAX'] = k['RANK0 MStencil/s  MAX'] 


    # add stencil operator
    k['stencil'] = get_stencil_num(k)
    if   k['stencil'] == 0:
      k['stencil_name'] = '25_pt_const'
    elif k['stencil'] == 1:
      k['stencil_name'] = '7_pt_const'
    elif k['stencil'] == 4:
      k['stencil_name']  = '25_pt_var'
    elif k['stencil'] == 5:
      k['stencil_name']  = '7_pt_var'
    elif k['stencil'] == 6:
      k['stencil_name']  = 'solar'


    # add the approach
    if(k['Time stepper orig name'] == 'Spatial Blocking'):
      k['method'] = 'Spt.blk.'
    elif(k['Time stepper orig name'] in ['PLUTO', 'Pochoir']):
      k['method'] = k['Time stepper orig name']
    elif(k['Time stepper orig name'] == 'Diamond'):
      if('_tgs1_' in k['file_name']):
        k['method'] = 'CATS2'
      else:
        k['method'] = 'MWD'
    else:
      print("ERROR: Unknow time stepper")
      raise

    # add mwd type
    k['mwdt']='none'
    if(k['method'] == 'MWD'):
      mwd = k['Wavefront parallel strategy'].lower()
      if('fixed' in mwd) and ('relaxed' in mwd):
        k['mwdt'] = 'fers'
      elif('fixed' in mwd):
        k['mwdt'] = 'fe'
      elif('relaxed' in mwd):
        k['mwdt'] = 'rs'
      elif('wavefront' in mwd):
        k['mwdt'] = 'block'


    # add precision information
    p = 1 if k['Precision'] in 'DP' else 0
    k['Precision'] = p


    # TLB measurement for LIKWID 4
    if 'L1 DTLB load miss rate avg' in k.keys():
      if k['L1 DTLB load miss rate avg']!='':
        hw_ctr_fields['TLB'] =  [('L1 DTLB load miss rate avg', float)]
        hw_ctr_labels['TLB'] =  [('L1 DTLB load miss rate avg', 'tlb_', 'tlb')]

    entry = {}
    # parse the general fileds' format
    for f in req_fields + hw_ctr_fields[k['LIKWID performance counter']]:
      try:
        entry[f[0]] = map(f[1], [k[f[0]]] )[0]
      except:
        print("ERROR: results entry missing essential data at file:%s"%(k['file_name']))
        print f[0]
        print k
        return

    #find repeated data
    key = (entry['Precision'], k['stencil_name'], k['LIKWID performance counter'], k['mwdt'], k['method'], entry['Global NX'])
    if key not in duplicates:
      duplicates.add(key)
    else:
      print("Repeated result at: %s"%(k['file_name']))
      continue


    # Initialize plot entry if does not exist for current data entry
#    for m,n in entry.iteritems(): print m,n
    measure_list = ['n', 'perf', 'total energy', 'tlb', 'mem bw', 'l2 bw', 'l3 bw', 'mem vol', 'l2 vol', 'l3 vol', 'data']
    plot_key = (entry['Precision'], k['stencil_name'], k['LIKWID performance counter'])
    line_key = (k['mwdt'], k['method'])
    if plot_key not in plots.keys():
      plots[plot_key] = {}
    if line_key not in plots[plot_key].keys():
      plots[plot_key][line_key] = {meas:[] for meas in measure_list}


    # append the data
    plots[plot_key][line_key]['n'].append(entry['Global NX'])
    plots[plot_key][line_key]['perf'].append(entry['MStencil/s  MAX']/1e3)
    N = entry['Global NX']**3 * entry['Number of time steps'] * entry['Number of tests']/1e9
    # Memory
    if k['LIKWID performance counter'] == 'MEM':
      plots[plot_key][line_key]['mem bw'].append(entry['Sustained Memory BW']/1e3)
      plots[plot_key][line_key]['mem vol'].append(entry['Total Memory Transfer']/N)
    # Energy
    elif k['LIKWID performance counter'] == 'ENERGY':
      entry['cpu energy pj/lup'] = entry['Energy']/N
      entry['dram energy pj/lup'] = entry['Energy DRAM']/N
      entry['total energy pj/lup'] = entry['cpu energy pj/lup'] + entry['dram energy pj/lup']
      if (entry['total energy pj/lup'] > 1e5): entry['total energy pj/lup'] = 0
      plots[plot_key][line_key]['total energy'].append(entry['total energy pj/lup'])
    # TLB
    elif k['LIKWID performance counter'] == 'TLB':
      plots[plot_key][line_key]['tlb'].append(entry[ hw_ctr_fields['TLB'][0][0] ])
    # L2
    elif k['LIKWID performance counter'] == 'L2':
      plots[plot_key][line_key]['l2 vol'].append(entry['L2 data volume sum']/N)
    #L3
    elif k['LIKWID performance counter'] == 'L3':
      plots[plot_key][line_key]['l3 vol'].append(entry['L3 data volume sum']/N)
    #CPU
    elif k['LIKWID performance counter'] == 'DATA':
      plots[plot_key][line_key]['data'].append(entry['Load to Store ratio avg'])
 
  del raw_data

  #sort the plot lines
  for p in plots:
    for l in plots[p]:
      pl = plots[p][l]
      #remove unused fields
      empty = []
      for key, val in pl.iteritems():
        if(val==[]):
          empty.append(key)
      for key in empty:
          del pl[key]
      lines = []
      [lines.append(pl[val]) for val in measure_list if val in pl.keys()]

      lines = sorted(zip(*lines))
      idx=0
      for val in measure_list:
        if(val in pl.keys()):
          if(pl[val]):
            pl[val] = [x[idx] for x in lines]
            idx = idx+1

#  for m,n in plots.iteritems(): 
#    print "##############",m
#    for i,j in n.iteritems():
#      print i,j

  for p in plots:
    plot_line(plots[p], stencil=p[1], plt_key=p[2])


def plot_line(p, stencil, plt_key):
  from operator import itemgetter
  import matplotlib.pyplot as plt
  import matplotlib
  import pylab
  from pylab import arange,pi,sin,cos,sqrt
  from scripts.utils import get_stencil_num

  m = 3.0
  fig_width = 4.0*0.393701*m # inches
  fig_height = 1.0*fig_width #* 210.0/280.0#433.62/578.16

  fig_size =  [fig_width,fig_height]
  params = {
         'axes.labelsize': 6*m,
         'axes.linewidth': 0.25*m,
         'lines.linewidth': 0.75*m,
         'font.size': 7*m,
         'legend.fontsize': 4*m,
         'xtick.labelsize': 6*m,
         'ytick.labelsize': 6*m,
         'lines.markersize': 1,
         'text.usetex': True,
         'figure.figsize': fig_size}
  pylab.rcParams.update(params)


  marker_s = 7
  line_w = 1
  line_s = '-' 
  method_style = {'Spt.blk.':('g','o'), 'MWD':('k','x'), 'CATS2':('r','+'),
                  'PLUTO':('m','*'), 'Pochoir':('b','^')}


  f_name = stencil+'_inc_grid_size'


  # performance
  plt.figure(0)
  for l in p:
    label = l[1]
    col, marker = method_style[label]
    x = p[l]['n']
    y_p = p[l]['perf']
    plt.plot(x, y_p, color=col, marker=marker, markersize=marker_s, linestyle=line_s, linewidth=line_w, label=label)

  plt.ylabel('GLUP/s')
  plt.grid()
  plt.xlabel('Size in each dimension')
  plt.legend(loc='best')
  pylab.savefig('perf_' + f_name + '_' + plt_key + '.pdf', format='pdf', bbox_inches="tight", pad_inches=0)
  plt.clf()


  # HW measurements
  plt_idx=1
  for y_label, file_prefix, measure in hw_ctr_labels[plt_key]:
    plt.figure(plt_idx)
    plt_idx = plt_idx + 1
    for l in p:
      label = l[1]
      col, marker = method_style[label]
      x = p[l]['n']
      y = p[l][measure]
      plt.plot(x, y, color=col, marker=marker, markersize=marker_s, linestyle=line_s, linewidth=line_w, label=label)

    plt.ylabel(y_label)
    plt.grid()
    plt.xlabel('Size in each dimension')
    plt.legend(loc='best')
    pylab.savefig(file_prefix + f_name+'.pdf', format='pdf', bbox_inches="tight", pad_inches=0)
    plt.clf()


if __name__ == "__main__":
  main()
