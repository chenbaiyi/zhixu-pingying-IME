"""Read-only prediction benchmark through the actual secured local service pipe.
Inputs are predeclared public synthetic cases. No learning observation is sent.
"""
import argparse,csv,ctypes,json,pathlib,re,struct,subprocess,time,uuid
import ime_pb2 as pb
from ctypes import wintypes
k=ctypes.WinDLL('kernel32',use_last_error=True);k.WaitNamedPipeW.argtypes=[wintypes.LPCWSTR,wintypes.DWORD]
def read_exact(f,n):
    out=b''
    while len(out)<n:
        chunk=f.read(n-len(out))
        if not chunk:raise RuntimeError('service disconnected')
        out+=chunk
    return out

def predict(name,query,allowed=True):
    session=uuid.uuid4().hex
    deadline=time.perf_counter()+2
    while not k.WaitNamedPipeW(name,100):
        if time.perf_counter()>=deadline:raise ctypes.WinError(ctypes.get_last_error())
        time.sleep(0.005)
    with open(name,'r+b',buffering=0) as f:
        def call(kind,payload,request_id):
            e=pb.Envelope(protocol_version=1,message_type=kind,session_id=session,request_id=request_id,context_version=1,payload=payload.SerializeToString())
            body=e.SerializeToString();f.write(struct.pack('<I',len(body))+body)
            size=struct.unpack('<I',read_exact(f,4))[0]
            if size>1024*1024:raise RuntimeError('response limit')
            response=pb.Envelope.FromString(read_exact(f,size))
            if response.request_id!=request_id or response.session_id!=session:raise RuntimeError('response identity')
            return response
        hello=call(pb.HELLO,pb.Hello(min_version=1,max_version=1),1)
        if hello.message_type!=pb.HELLO_ACK:raise RuntimeError('handshake rejected')
        request=pb.PredictionRequest(context_text=query,candidate_limit=3)
        request.privacy.allow_prediction=allowed
        response=call(pb.PREDICTION_REQUEST,request,2)
        if response.message_type!=pb.PREDICTION_RESULT:raise RuntimeError('wrong response type')
        result=pb.PredictionResult.FromString(response.payload)
        return [{'text':c.text,'source':c.source,'package':c.package_id} for c in result.candidates]

class Memory(ctypes.Structure):
    _fields_=[('cb',wintypes.DWORD),('PageFaultCount',wintypes.DWORD)]+[(x,ctypes.c_size_t) for x in ['PeakWorkingSetSize','WorkingSetSize','QuotaPeakPagedPoolUsage','QuotaPagedPoolUsage','QuotaPeakNonPagedPoolUsage','QuotaNonPagedPoolUsage','PagefileUsage','PeakPagefileUsage','PrivateUsage']]
def working_set(pid):
    k.OpenProcess.restype=wintypes.HANDLE;k.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
    handle=k.OpenProcess(0x410,False,pid)
    if not handle:raise ctypes.WinError(ctypes.get_last_error())
    psapi=ctypes.WinDLL('psapi',use_last_error=True);psapi.GetProcessMemoryInfo.argtypes=[wintypes.HANDLE,ctypes.c_void_p,wintypes.DWORD]
    m=Memory();m.cb=ctypes.sizeof(m)
    if not psapi.GetProcessMemoryInfo(handle,ctypes.byref(m),m.cb):raise ctypes.WinError(ctypes.get_last_error())
    k.CloseHandle.argtypes=[wintypes.HANDLE];k.CloseHandle(handle);return m.WorkingSetSize

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--role',required=True);p.add_argument('--pid',type=int,required=True);p.add_argument('--cases',type=pathlib.Path,required=True);p.add_argument('--report',type=pathlib.Path,required=True);a=p.parse_args()
    sid=re.search(r'S-1-5-[0-9-]+',subprocess.check_output(['whoami','/user','/fo','csv','/nh']).decode(errors='replace')).group()
    name='\\\\.\\pipe\\ZhixuIME.'+a.role+'.'+sid
    cases=json.loads(a.cases.read_text(encoding='utf-8'));timings=[];results=[]
    for pass_number in range(4):
        for case in cases:
            start=time.perf_counter();options=predict(name,case['query']);elapsed=(time.perf_counter()-start)*1000
            if pass_number:timings.append(elapsed)
            else:results.append({'query':case['query'],'suggestions':options,'accepted_match':any(o['text'] in case['acceptable'] for o in options),'expected_suppression':not case['acceptable'] and not options})
        print('SERVICE_PASS',pass_number,flush=True)
    protected=predict(name,'大地一片昏暗',False)
    timings.sort();p95=timings[len(timings)*95//100]
    report={'samples':len(timings),'service_rpc_p50_ms':timings[len(timings)//2],'service_rpc_p95_ms':p95,'estimated_total_p95_including_100ms_debounce':p95+100,'actual_application_latency_verified':False,'service_working_set_bytes':working_set(a.pid),'usable_top3_cases':sum(r['accepted_match'] for r in results),'low_relevance_suppressed_cases':sum(r['expected_suppression'] for r in results),'protected_request_suppressed':not protected,'results':results}
    a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='results'},ensure_ascii=False),flush=True)
