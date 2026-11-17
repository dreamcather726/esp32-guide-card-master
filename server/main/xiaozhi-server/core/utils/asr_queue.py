import asyncio
from dataclasses import dataclass, field


@dataclass
class AudioQueueBarrier:
    done: asyncio.Event = field(default_factory=asyncio.Event)


async def drain_audio_input(conn):
    """Wait for preceding wire-order frames, including the worker's in-flight frame."""
    barrier = AudioQueueBarrier()
    conn.asr_audio_queue.put_nowait(barrier)
    await asyncio.wait_for(barrier.done.wait(), timeout=15)
