tmux new-session -d -s main ;
tmux send-keys -t main "./t4p4s.sh :$1" C-m
tmux split-window -v ;
tmux select-pane -2

tmux split-window -h ;
tmux select-pane -L
tmux send-keys -t main 'sudo ip netns exec red iperf -s 10.10.10.2' C-m
tmux select-pane -R
tmux send-keys -t main 'sudo ip netns exec blue iperf -c 10.10.10.2' C-m
tmux attach-session -d -t main 

