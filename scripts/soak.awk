# Bounded scan (1441 samples/day). Only the newest sample emits events.
# ponytail: O(n) per minute; incremental state only if multi-week runs are needed.
BEGIN {FS=","; names[1]="ATPD"; names[2]="sing-box"}
function event(kind, detail, failure) {
    count[kind]++
    if (NR == emit+1 && emit>0) {
        print $1 " " kind " " detail >> (out "/events.log")
        if (failure) print $1 " " kind " " detail >> (out "/failures.log")
    }
}
function range(key,value) {
    if (value !~ /^[0-9]+$/) return
    if (!(key in minimum)) {minimum[key]=maximum[key]=first[key]=value+0}
    if (value<minimum[key]) minimum[key]=value+0
    if (value>maximum[key]) maximum[key]=value+0
    last[key]=value+0
}
function process(p,offset, pid,birth,alive,rss,fd,threads,ticks,id,k,same) {
    pid=$(offset); birth=$(offset+1); alive=$(offset+2); rss=$(offset+3)
    fd=$(offset+4); threads=$(offset+5); ticks=$(offset+6)
    k=names[p]; id=pid ":" birth
    if (pid ~ /^[0-9]+$/ && previous_pid[p]!="" && pid!=previous_pid[p]) event(k "_pid_change",previous_pid[p] " -> " pid,0)
    if (pid ~ /^[0-9]+$/) previous_pid[p]=pid
    if (alive=="0" && previous_alive[p]!="0") event(k "_disappearance","pid=" pid,1)
    if (alive=="UNKNOWN" && previous_alive[p]!="UNKNOWN") event(k "_observation_lost","PID missing, mismatched, or /proc race",0)
    same=(alive=="1" && previous_alive[p]=="1" && id==identity[p])
    if (alive=="1") {
        if (identity[p]!="" && id!=identity[p]) event(k "_unexpected_restart","observed new identity " id "; cause unknown (may be user initiated)",1)
        range(k " RSS_kB",rss); range(k " FD",fd); range(k " threads",threads)
        if (same) {
            if (fd ~ /^[0-9]+$/ && oldfd[p] ~ /^[0-9]+$/ && fd-oldfd[p]>=32 && fd>=oldfd[p]*1.5) event(k "_fd_growth",oldfd[p] " -> " fd,0)
            if (threads ~ /^[0-9]+$/ && oldthreads[p] ~ /^[0-9]+$/ && threads-oldthreads[p]>=8 && threads>=oldthreads[p]*1.5) event(k "_thread_growth",oldthreads[p] " -> " threads,0)
            if (rss ~ /^[0-9]+$/ && oldrss[p] ~ /^[0-9]+$/ && rss>oldrss[p]) {
                if (!streak[p]) base[p]=oldrss[p]
                streak[p]++
                if (streak[p]>=10 && rss-base[p]>=4096 && rss>=base[p]*1.1 && !warned[p]) {
                    event(k "_rss_growth","10+ consecutive increases, >=4MiB and >=10%; heuristic, not proof of leak",0)
                    warned[p]=1
                }
            } else {streak[p]=0; warned[p]=0}
        } else {streak[p]=0; warned[p]=0}
        identity[p]=id
        oldrss[p]=rss; oldfd[p]=fd; oldthreads[p]=threads
    } else {streak[p]=0; warned[p]=0}
    previous_alive[p]=alive
}
NR==1 {next}
{
    samples++; if (samples==1) begin=$2
    end=$2
    process(1,3); process(2,10)
    if ($19=="unhealthy") {health++; if (prevhealth!=$19) event("health_failure","sing-box unhealthy",1)}
    if ($20=="INVALID") {invalid++; if (prevapi!=$20) event("native_api_invalid","owner snapshot invalid",1)}
    if ($20=="UNKNOWN") unknown++
    if ($18=="FAILED" && prevservice!="FAILED") event("service_FAILED","sing-box",1)
    if ($23!=0) {statusfail++; event("status_failure","client exit=" $23,1)}
    if ($21!="UNKNOWN") {
        vpn=$21 ":" $22
        if (prevvpn!="" && prevvpn!=vpn) event("vpn_transition",prevvpn " -> " vpn,0)
        prevvpn=vpn
    }
    prevhealth=$19; prevapi=$20; prevservice=$18
}
END {
    print "Actual duration seconds: " (samples ? now-begin : 0)
    print "Observed sample span seconds: " (samples ? end-begin : 0)
    print "Total samples: " samples
    for (p=1;p<=2;p++) {
        k=names[p]
        print k " PID changes: " count[k "_pid_change"]+0
        print k " unexpected restart observations: " count[k "_unexpected_restart"]+0
        for (j=1;j<=3;j++) {
            metric=(j==1 ? " RSS_kB" : (j==2 ? " FD" : " threads")); key=k metric
            if (key in minimum) print key " min/max/delta: " minimum[key] "/" maximum[key] "/" last[key]-first[key]
            else print key " min/max/delta: UNKNOWN"
        }
        print k " sustained RSS growth warnings: " count[k "_rss_growth"]+0
    }
    print "Health failure samples/episodes: " health+0 "/" count["health_failure"]+0
    print "Native API invalid samples/episodes: " invalid+0 "/" count["native_api_invalid"]+0
    print "Native API unknown samples: " unknown+0
    print "VPN transitions: " count["vpn_transition"]+0
    print "Sustained growth detected: " ((count["ATPD_rss_growth"]+count["sing-box_rss_growth"]>0) ? "YES" : "NO (within observed valid samples)")
    print "Status query failures: " statusfail+0
    print "CPU: raw utime+stime ticks in CSV; CLK_TCK=" hz " (0=unavailable)."
    print "Limits: runtime RUNNING/STOPPED and VPN READY/NOT_READY are status projections."
    print "Native API validity inferred from owner-snapshot Goroutines; missing field=UNKNOWN."
    print "PID/starttime identifies restarts; user actions cannot be distinguished from faults."
    print "RSS delta spans process generations; trend detection resets on identity change/gaps."
    print "No wakelock; suspend may delay sampling. Warnings are observations, not leak diagnoses."
}
