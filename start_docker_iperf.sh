tmux new-session -d -s main ;
tmux send-keys -t main "./t4p4s.sh :$1 verbose" C-m
#tmux send-keys -t main "./t4p4s.sh :$1 verbose > ~/t4p4s.log" C-m
#tmux send-keys -t main "./t4p4s.sh :$1 verbose > /tmp/t4p4s.log& tail -f /tmp/t4p4s.log" C-m
tmux split-window -v ;
tmux select-pane -2

tmux split-window -h ;
tmux select-pane -L
tmux send-keys -t main 'sudo ip netns exec red iperf3 -s 10.10.10.2' C-m
tmux select-pane -R
tmux send-keys -t main 'sudo ip netns exec blue iperf3 -t 120 -c 10.10.10.2 -P 20 -i 1 --logfile ~/iperf-log.txt' C-m
tmux attach-session -d -t main 

