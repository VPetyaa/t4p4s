import time
from optparse import OptionParser
from influxdb import InfluxDBClient
import threading
import sys
import logging
import subprocess

DBNAME='p4_aqm'

client = InfluxDBClient(database=DBNAME)


SLEEP_INTERVAL = 1.0

def process_line(line):
    lv = line.rstrip().split()
    #buffer size:    qs      qs/1000 ig_dr   q_dr    av_qd/K queue_latency   rx_rate maxqB/k
    #buffer size:    0       0       0       0       0       0.000000        0       0

    if len(lv) < 5:
        print("bad line" + line )
        return
    if lv[0] == "buffer" and lv[1] == "size:":
        print("New line possibly t4p4s")
        if lv[2] != "qs":
            print("New t4p4s line")
            inst_depth = int(lv[2])
            inst_depth_k = int(lv[3])
            ingress_droprate = int(lv[4])
            queue_droprate = int(lv[5])
            average_qd = int(lv[6])
            inst_delay = float(lv[7]) #ms
            packet_rate = int(lv[8])
            client.write( ['inst_depth,mode=inst_depth value=%d' % (inst_depth)], {'db':DBNAME},204,'line')
            client.write( ['inst_depth_k,mode=inst_depth_k value=%d' % (inst_depth_k)], {'db':DBNAME},204,'line')
            client.write( ['ingress_droprate,mode=ingress_droprate value=%d' % (ingress_droprate)], {'db':DBNAME},204,'line')
            client.write( ['queue_droprate,mode=queue_droprate value=%d' % (queue_droprate)], {'db':DBNAME},204,'line')
            client.write( ['average_qd,mode=average_qd value=%d' % (average_qd)], {'db':DBNAME},204,'line')
            client.write( ['inst_delay,mode=inst_delay value=%f' % (inst_delay)], {'db':DBNAME},204,'line')
            client.write( ['packet_rate,mode=packet_rate value=%d' % (packet_rate)], {'db':DBNAME},204,'line')
            print("T4P4S line added\n")
    elif lv[0] == "[SUM]":
        print("New iperf sum")
        flow_id = 99999
        if int(round(float(lv[1].split('-')[1])))-int(round(float(lv[1].split('-')[0]))) > 1:
            print("iperf line is summary")
            return
        rate = float(lv[3])
        if lv[4] == 'GBytes':
            rate *= 1000
        bandwidth = float(lv[5])
        if lv[6] == 'Gbits/sec':
            bandwidth*=1000
        client.write( ['bandwidth,mode=flow_id%d value=%d' % (flow_id,bandwidth)], {'db':DBNAME},204,'line')
        client.write( ['iperf_rate,mode=flow_id%d value=%f' % (flow_id,rate)], {'db':DBNAME},204,'line')
    elif lv[0] == "[":
        print("New iperf line")
        flow_id = 0
        try:
            flow_id = int(lv[1][:-1])
        except:
            return
        if int(round(float(lv[2].split('-')[1])))-int(round(float(lv[2].split('-')[0]))) > 1:
            print("iperf line is summary")
            return
        rate = float(lv[4])
        if lv[5] == 'GBytes':
            rate *= 1000
        bandwidth = float(lv[6])
        if lv[7] == 'Gbits/sec':
            bandwidth*=1000
        client.write( ['bandwidth,mode=flow_id%d value=%d' % (flow_id,bandwidth)], {'db':DBNAME},204,'line')
        client.write( ['iperf_rate,mode=flow_id%d value=%f' % (flow_id,rate)], {'db':DBNAME},204,'line')
        if lv[8] != 'receiver':
            retransmission = int(lv[8]) #ms
            client.write( ['retransmission,mode=flow_id%d value=%d' % (flow_id,retransmission)], {'db':DBNAME},204,'line')
        print("Iperf line added")



def readlines_then_tail(fin):
    "Iterate through lines and then tail for further lines."
    while True:
        line = fin.readline()
        if line:
            yield line
        else:
            tail(fin)

def tail(fin):
    "Listen for new lines added to file."
    while True:
        where = fin.tell()
        line = fin.readline()
        if not line:
            time.sleep(SLEEP_INTERVAL)
            fin.seek(where)
        else:
            yield line

SHAPER_IP='vpetya@192.168.0.102'
#SHAPER_IP='vpetya@192.168.0.101'
client = InfluxDBClient(database=DBNAME)

def thread_function(name='SSH'):
    print("Thread "+name+": starting")
    #bashCommand = ["ssh", SHAPER_IP, "tail -f ~/root/iperf-log.txt"]
    bashCommand = ["tail", "-f",  "/home/vpetya/root/iperf-log.txt"]
    try:
        process = subprocess.Popen(bashCommand, stdout=subprocess.PIPE)
        while True:
            nextline = (process.stdout.readline()).decode("utf-8")
            if nextline == '' and process.poll() is not None:
                time.sleep(0.001)
                #output = process.communicate()[0].split('\n')
                #for i in output:
                #    process_line(i)
            try:
                x = threading.Thread(target=process_line, args=(nextline,))
                x.daemon = True
                x.start()
                #process_line(nextline)
            except Exception as err:
                print ("Error:", err, " -- ", nextline)
    except Exception as err:
        print ("Error:", err, " -- ")
    logging.info("Thread %s: finishing", name)

def thread_function_file(name='File'):
    print("Thread "+name+" starting")
    bashCommand = ["tail", "-f",  sys.argv[1]]
    try:
        process = subprocess.Popen(bashCommand, stdout=subprocess.PIPE)
        while True:
            nextline = (process.stdout.readline()).decode("utf-8")
            if nextline == '' and process.poll() is not None:
                time.sleep(0.001)
                #output = process.communicate()[0].split('\n')
                #for i in output:
                #    process_line(i)
            try:
                #process_line(nextline)
                x = threading.Thread(target=process_line, args=(nextline,))
                x.daemon = True
                x.start()
            except Exception as err:
                print ("Error:", err, " -- ")
    except Exception as err:
        print ("Error:", err, " -- ")


x = threading.Thread(target=thread_function)
x.daemon = True
x.start()

x = threading.Thread(target=thread_function_file)
x.daemon = True
x.start()

import time
while True:
    time.sleep(1)


"""
def main():
    p = OptionParser("usage: tail.py file")
    (options, args) = p.parse_args()
    if len(args) < 1:
        p.error("must specify a file to watch")
    with open(args[0], 'r') as fin:
        for line in readlines_then_tail(fin):
            print (line.strip())
            process_line(line)

if __name__ == '__main__':
    main()
"""
