"""Serving consecutive chunks through a bus response queue (bus.ResponseQueue)."""

from collections import deque

import protocol as p
from bus import ResponseQueue


class ChunkStream:
    """Serves the chunks of @p data as the response of @p lin_id: the chunk at the restart offset
    is the current response, the following ones are queued, and the queue is kept filled up to a
    limit as the bus reports the published ones."""

    def __init__(self, bus: ResponseQueue, lin_id: int, data: bytes):
        self.bus = bus
        self.lin_id = lin_id
        self.data = data
        # The offsets of the chunks given to the bus and not published yet (the first one is the
        # current response), of the next chunk to queue, and the end of what may be queued.
        self.in_bus: deque[int] = deque()
        self.next_offset = 0
        self.end = 0

    def restart(self, offset: int, end: int) -> None:
        """Serves from @p offset on, queuing the chunks before @p end."""
        # Also makes the bus report the publishes (even if nothing gets queued).
        self.bus.replace_response(self.lin_id, p.chunk_frame(self.data, offset))
        self.in_bus = deque([offset])
        self.next_offset = offset + p.CHUNK_SIZE
        self.extend(end)

    def extend(self, end: int) -> None:
        """Queues the chunks before @p end, as far as the queue has room."""
        self.end = end
        while len(self.in_bus) <= self.bus.response_queue_depth and self.next_offset < self.end:
            if not self.bus.queue_response(self.lin_id, p.chunk_frame(self.data, self.next_offset)):
                break
            self.in_bus.append(self.next_offset)
            self.next_offset += p.CHUNK_SIZE

    def published(self, data: bytes) -> int | None:
        """The bus reports that the response @p data was published (after restart()). Returns the
        offset of its chunk, or None if it was published before the last restart()."""
        offset = self.in_bus[0]
        if data != p.chunk_frame(self.data, offset):
            return None
        if len(self.in_bus) > 1:
            # The next queued chunk is now the current response.
            self.in_bus.popleft()
        self.extend(self.end)
        return offset
