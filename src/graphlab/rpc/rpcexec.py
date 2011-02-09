#!/usr/bin/python
import sys
import os
import string
import subprocess
import time

"""
Usage: rpcexec -n n_to_start -f [hostsfile] [program] [options]
To start local only: rpcexec [program] [options]
"""

def escape(s):
  s = string.replace(s, '"', '\\"')
  s = string.replace(s, "'", "\\'")
  return s
#enddef

# gui: if xterm should run
# machines: a vector of all the machines
# port: a vector of the port number for ssh to connect to. must be same length as machines
# machineid: The machineid to generate
# prog: program to run
# opts: options for the program
def get_ssh_cmd(gui, machines, port, machineid, prog, opts):

  allmachines = '"' + string.join(machines, ',') + '"'

  # construct the command line
  cwd = os.getcwd()
  if (gui):
    sshcmd = 'ssh -X -Y -n -q '
  else:
    sshcmd = 'ssh -n -q '
  #endif

  guicmd = ''
  if (gui):
    guicmd = 'xterm -geometry 120x60 -e '
  #endif

  if (machines[i] == "localhost" or machines[i].startswith("127.")):
    cmd = 'env SPAWNNODES=%s SPAWNID=%d %s %s' % (allmachines,i, prog, opts)
  elif (port[i] == 22):
    cmd = sshcmd + '%s "cd %s ; env SPAWNNODES=%s SPAWNID=%d %s %s %s"' %                       \
                    (machines[machineid], escape(cwd), escape(allmachines),machineid,           \
                    guicmd, escape(prog), escape(opts))
  else:
    cmd = sshcmd + '-oPort=%d %s "cd %s ; env SPAWNNODES=%s SPAWNID=%d %s %s %s"' %              \
                    (port[machineid], machines[machineid], escape(cwd), escape(allmachines),     \
                    machineid, guicmd, escape(prog), escape(opts))
  #endif
  return cmd
#enddef



def get_screen_cmd(gui, machines, port, machineid, prog, opts):

  allmachines = '"' + string.join(machines, ',') + '"'

  # construct the command line
  cwd = os.getcwd()
  if (gui):
    sshcmd = 'ssh -X -Y -n -q '
  else:
    sshcmd = 'ssh -n -q '
  #endif

  guicmd = ''
  if (gui):
    guicmd = 'xterm -geometry 120x60 -e '
  #endif

  if (machines[i] == "localhost" or machines[i].startswith("127.")):
    cmd = 'export SPAWNNODES=%s SPAWNID=%d ; %s %s' % (allmachines,i, prog, opts)
  elif (port[i] == 22):
    cmd = sshcmd + '%s "cd %s ; export SPAWNNODES=%s SPAWNID=%d ; %s %s %s"' %                       \
                    (machines[machineid], cwd, allmachines,machineid,           \
                    guicmd, prog, opts)
  else:
    cmd = sshcmd + '-oPort=%d %s "cd %s ; export SPAWNNODES=%s SPAWNID=%d ; %s %s %s"' %              \
                    (port[machineid], machines[machineid], cwd, allmachines,     \
                    machineid, guicmd, prog, opts)
  #endif
  return "'" + cmd + "\n'"
#enddef


def shell_popen(cmd):
  print cmd
  return subprocess.Popen(cmd, shell=True)
#endif

def shell_wait_native(cmd):
  print cmd
  pid = subprocess.Popen(cmd, shell=True,executable='/bin/bash')
  os.waitpid(pid.pid, 0)
#  os.system(cmd)
#  time.sleep(0.5)
#endif


nmachines = 0
hostsfile = ''
prog = ''
opts = ''
gui = 0
inscreen = 0
screenname = ''
printhelp = 0
i = 1
while(i < len(sys.argv)):
  if sys.argv[i] == '-h' or sys.argv[i] == '--help':
    printhelp = 1
    break
  elif sys.argv[i] == '-n':
    nmachines = int(sys.argv[i+1])
    i = i + 2
  elif sys.argv[i] == '-f':
    hostsfile = sys.argv[i+1]
    i = i + 2
  elif sys.argv[i] == '-g':
    gui = 1
    i = i + 1
  elif sys.argv[i] == '-s':
    inscreen = 1
    screenname = sys.argv[i+1]
    i = i + 2
  else:
    prog = sys.argv[i]
    if (len(sys.argv) > i+1):
      opts = string.join(sys.argv[(i+1):])
    #endif
    break
  #endif
#endwhile
if inscreen and gui:
  print ("-s and -g are mutually exclusive")
  exit(0)
#endif

if (printhelp):
  print
  print("Usage: rpcexec -n n_to_start -f [hostsfile] [program] [options]")
  print("To start local only: rpcexec [program] [options]")
  print("Optional Arguments:")
  print("-g: Launch the command within Xterm on all machines. ")
  print("-s [screenname] : Launch a shell_popen session and launch the commands in each window in the shell_popen")
  print("")
  print("-s and -g are mutually exclusive")
  
  exit(0)
#endif

if (nmachines == 0 and hostsfile == ''):
  cmd = 'env SPAWNNODES=localhost SPAWNID=0 %s %s' % (prog, opts)
  p = subprocess.Popen(cmd, shell_popen=True)
  os.waitpid(p.pid, 0)
  exit(0)
#endif
print('Starting ' + str(nmachines) + ' machines')
print('Hosts file: ' + hostsfile)
print('Command Line to run: ' + prog + ' ' + opts)





# open the hosts file and read the machines
try:
  f = open(hostsfile, 'r')
except:
  print
  print("Unable to open hosts file")
  print
  exit(0)
#endtry

machines = [''] * nmachines
port = [22] * nmachines
for i in range(nmachines):
  try:
    machines[i] = string.strip(f.readline())
    colonsplit = string.split(machines[i], ':')
    if (len(colonsplit) == 2):
      machines[i] = string.strip(colonsplit[0])
      port[i] = int(colonsplit[1])
    #endif
  except:
    print
    print("Unable to read line " + str(i+1) + " of hosts file")
    print
    exit(0)
#endfor
f.close()

# the commands to run to start for each node
cmd = [None] * nmachines
for i in range(nmachines):
  if (inscreen == 0):
    cmd[i] = get_ssh_cmd(gui, machines, port, i, prog, opts)
  else:
    cmd[i] = get_screen_cmd(gui, machines, port, i, prog, opts)
  #endif
#endfor

if (inscreen == 0):
  # now issue the ssh commands
  procs = [None] * nmachines
  for i in range(nmachines):
    procs[i] = shell_popen(cmd[i])
  #endfor
  
  for i in range(nmachines):
    os.waitpid(procs[i].pid, 0)
  #endfor
else:
  # create a new empty screen with the screen name
  shell_wait_native("screen -d -m -S " + screenname)
  shell_wait_native("screen -x %s -p 0 -X title %s" % (screenname, machines[0][0:8]))

  # start a bunch of empty screens
  for i in range(nmachines - 1):
    shell_wait_native("screen -x %s -X screen -t %s" % (screenname, machines[i+1][0:8]))
  #endfor
  # set the titles in each one and run the program
  for i in range(nmachines):
    shell_popen("screen -x %s -p %d -X stuff %s" % (screenname, i, cmd[i]))
  #endfor
#endif