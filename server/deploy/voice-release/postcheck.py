"""Sanitized acceptance metrics since the current voice process started."""
import json
from pathlib import Path
import re
import subprocess

data = json.loads(subprocess.check_output(['docker', 'inspect', 'xiaozhi-esp32-server'], text=True))[0]
logs = subprocess.run(['docker', 'logs', '--since', data['State']['StartedAt'], 'xiaozhi-esp32-server'],
    capture_output=True, text=True, check=True)
text = logs.stdout + logs.stderr
faults = ['model_not_found', "'NoneType' object has no attribute 'interface_type'",
          "'NoneType' object has no attribute 'close'", 'Waiting for audio queue completion timed out',
          'event=failed reason=', 'reason=stalled', 'reason=sender_failed', 'initialization_error=',
          'input_drain_error=', 'TTS output timeout']
result = {
    'running': data['State']['Running'], 'oom_killed': data['State']['OOMKilled'],
    'restart_count': data['RestartCount'], 'started_at_utc': data['State']['StartedAt'],
    'image': data['Image'],
    'target_fault_counts': {marker: text.count(marker) for marker in faults},
    'asr_final_events': len(re.findall(r'\[ASR_DIAG\] event=final ', text)),
    'asr_failed_events': len(re.findall(r'\[ASR_DIAG\] event=failed ', text)),
    'completed_audio_turns': len(re.findall(r'\[TTS_DIAG\].*reason=complete', text)),
    'llm_models': sorted(set(re.findall(r'\[LLM_DIAG\] request_start model=(\S+)', text))),
}
assert result['running'] and not result['oom_killed'] and result['restart_count'] == 0
assert not any(result['target_fault_counts'].values()) and not result['asr_failed_events']
Path(__file__).with_name('postcheck-result.json').write_text(json.dumps(result, indent=2))
print(json.dumps(result))
