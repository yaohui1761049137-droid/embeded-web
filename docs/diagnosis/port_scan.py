"""Async full TCP port scan of the board (fast, proactor)."""
import asyncio
import sys

HOST = "192.168.1.111"
CONCURRENCY = 800
TIMEOUT = 1.5
SEMAPHORE = asyncio.Semaphore(CONCURRENCY)


async def probe(port: int, results: list):
    async with SEMAPHORE:
        try:
            fut = asyncio.open_connection(HOST, port)
            reader, writer = await asyncio.wait_for(fut, timeout=TIMEOUT)
            results.append(port)
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass
        except (OSError, asyncio.TimeoutError):
            pass


async def main():
    results = []
    tasks = [probe(p, results) for p in range(1, 65536)]
    await asyncio.gather(*tasks)
    results.sort()
    print("open ports:", results or "NONE")


if __name__ == "__main__":
    if sys.platform == "win32":
        asyncio.set_event_loop_policy(asyncio.WindowsProactorEventLoopPolicy())
    asyncio.run(main())
