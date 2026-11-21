"""Experimental loopback-only ADB relay for a bounded transport comparison."""
import asyncio
import ipaddress
import sys

async def pump(reader, writer):
    while data := await reader.read(262144):
        writer.write(data)
        await writer.drain()
    writer.write_eof()

async def main():
    target = ipaddress.IPv4Address(sys.argv[1])
    if not target.is_private:
        raise ValueError("expected container address")
    async def relay(reader, writer):
        upstream = None
        try:
            other, upstream = await asyncio.wait_for(asyncio.open_connection(str(target), 5555), 3)
            async with asyncio.TaskGroup() as g:
                g.create_task(pump(reader, upstream))
                g.create_task(pump(other, writer))
        finally:
            for w in (upstream, writer):
                if w is not None:
                    w.close()
    async with await asyncio.start_server(relay, '127.0.0.1', int(sys.argv[2])) as s:
        await s.serve_forever()

if __name__ == '__main__':
    asyncio.run(main())
