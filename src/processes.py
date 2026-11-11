import os
import queue
import subprocess
import threading
import time

def parse_result(text):
    result = {}
    milestone = None
    active = False
    for line in text.splitlines():
        if line.startswith("TPATH_RESPONSE_V1\t"):
            milestone = line.split("\t")[-1]
        elif line == "TPATH_RESULT_V1":
            active = True
        elif active and "\t" in line:
            key, value = line.split("\t", 1)
            result[key] = value
    if milestone is not None and not result.get("response_1000_ms"):
        result.update(response_1000_ms=milestone, response_reached="1")
    return result

def run_process(command, timeout, allow_empty=False):
    started = time.monotonic()
    process = subprocess.Popen(list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    timed_out = False
    try:
        try:
            output, error = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()
            output, error = process.communicate()
    except BaseException:
        process.kill()
        process.communicate()
        raise
    result = parse_result(output)
    if timed_out:
        result["status"] = "TIMEOUT_HARD"
    elif not result.get("status"):
        result["status"] = "OK" if allow_empty and process.returncode == 0 else "PROCESS_ERROR"
    result.update(_pid=process.pid, _return_code=process.returncode, _stdout=output, _stderr=error,
                  _process_wall_ms_excluded=1000 * (time.monotonic() - started))
    return result

class QueryProcess:

    def __init__(self, executable, cache, error_file, setup_timeout=1200):
        self.error_stream = open(error_file, "a", encoding="utf-8")
        self.process = subprocess.Popen([str(executable), "--graph-cache", str(cache)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.error_stream, text=True, encoding="utf-8", bufsize=1)
        self.messages = queue.Queue()
        self.reader = threading.Thread(target=self._read_output, daemon=True)
        self.reader.start()
        try:
            ready = self.messages.get(timeout=setup_timeout)
            if ready is None or not ready.startswith("TPATH_READY\t"):
                raise RuntimeError("worker did not load graph: " + str(ready))
            self.graph_metadata = ready.split("\t")[1:]
        except BaseException:
            self.close()
            raise

    def _read_output(self):
        for line in self.process.stdout:
            self.messages.put(line.rstrip("\r\n"))
        self.messages.put(None)

    def request(self, action, values, timeout):
        arguments = ["--action", action]
        for key, value in values.items():
            arguments.extend(["--" + key, str(value)])
        if any("\t" in value or "\n" in value for value in arguments):
            raise ValueError("tabs and newlines are not supported in native arguments")
        self.process.stdin.write("\t".join(arguments) + "\n")
        self.process.stdin.flush()
        started = time.monotonic()
        output = []
        try:
            while True:
                remaining = timeout - (time.monotonic() - started)
                if remaining <= 0:
                    raise queue.Empty()
                line = self.messages.get(timeout=remaining)
                if line is None:
                    result = parse_result("\n".join(output))
                    result.update(status="OOM" if self.process.wait() == 3 else "PROCESS_ERROR")
                    break
                output.append(line)
                if line == "TPATH_END":
                    result = parse_result("\n".join(output))
                    if "status" not in result:
                        raise RuntimeError("worker returned no status")
                    break
        except queue.Empty:
            self.close()
            result = parse_result("\n".join(output))
            result["status"] = "TIMEOUT_HARD"
        result.update(_pid=self.process.pid, _stdout="\n".join(output),
                      _request_wall_ms_excluded=1000 * (time.monotonic() - started))
        return result

    def close(self):
        if self.process.poll() is None:
            try:
                self.process.stdin.write("QUIT\n")
                self.process.stdin.flush()
                self.process.wait(timeout=2)
            except (OSError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait()
        self.reader.join(timeout=2)
        self.error_stream.close()
        if self.process.stdin:
            self.process.stdin.close()
        if self.process.stdout:
            self.process.stdout.close()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()
