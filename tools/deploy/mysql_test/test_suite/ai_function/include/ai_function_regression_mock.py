#!/usr/bin/env python3
"""Local regression fixture written by this team, not the contest's official mock.

Run alongside seekdb: python3 ai_function_regression_mock.py --port 8000
Only synthetic request bodies are recorded; authentication headers are excluded.
"""

import argparse
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


def embedding(text, dimension=6):
    digest = hashlib.sha256(text.encode()).digest()
    return [round(digest[i % len(digest)] / 256 + i / 100, 6) for i in range(dimension)]


class Handler(BaseHTTPRequestHandler):
    requests = []
    lock = threading.Lock()
    default_dimension = 6

    def log_message(self, *_):
        pass

    def reply(self, value, status=200):
        body = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/requests":
            with self.lock:
                self.reply(list(self.requests))
        else:
            self.reply({"status": "ready", "fixture": "team-authored"})

    def do_DELETE(self):
        with self.lock:
            self.requests.clear()
        self.reply({"cleared": True})

    def do_POST(self):
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= 64 * 1024 * 1024:
                raise ValueError("invalid body size")
            body = json.loads(self.rfile.read(size))
            url = urlsplit(self.path)
            case = parse_qs(url.query).get("case", [""])[0]
            with self.lock:
                self.requests.append({"path": self.path, "body": body})
            if url.path.endswith(("/completions", "/generate")):
                if url.path.endswith("/generate"):
                    if body.get("stream") is not False:
                        raise ValueError(
                            "native Ollama completion requires stream=false"
                        )
                    if self.headers.get_content_type() != "application/json":
                        raise ValueError(
                            "native Ollama completion needs JSON content type"
                        )
                    text = body["prompt"]
                else:
                    text = body["messages"][-1]["content"]
                value = {"empty": "", "null": None, "object": {"x": 1}}.get(case, text)
                response = (
                    {"response": value}
                    if url.path.endswith("/generate")
                    else {"choices": [{"message": {"content": value}}]}
                )
                if case == "missing":
                    response = {}
            elif url.path.endswith(("/embeddings", "/embed")):
                inputs = body["input"]
                if not isinstance(inputs, list):
                    raise ValueError("input must be an array")
                dimension = body.get("dimensions", self.default_dimension)
                if not isinstance(dimension, int) or not 0 < dimension <= 16384:
                    raise ValueError("invalid dimension")
                vectors = [embedding(text, dimension) for text in inputs]
                if case == "bad_element":
                    vectors[0][0] = "invalid"
                elif case == "bad_dimension":
                    vectors[0].append(0)
                elif case == "empty_vector":
                    vectors[0] = []
                elif case == "vector_object":
                    vectors[0] = {"x": 1}
                if url.path.endswith("/embed"):
                    response = {"embeddings": vectors}
                else:
                    data = [
                        {"index": i, "embedding": value}
                        for i, value in enumerate(vectors)
                    ]
                    if case == "duplicate_index" and len(data) > 1:
                        data[-1]["index"] = 0
                    elif case == "bad_index":
                        data[0]["index"] = -1
                    elif case == "missing_index":
                        del data[0]["index"]
                    elif case == "missing_row":
                        data.pop()
                    response = {"data": list(reversed(data))}
            elif url.path.endswith("/rerank"):
                documents = body["documents"]
                results = []
                for i, text in enumerate(documents):
                    if body["query"] == "ties":
                        score = 1.0
                    else:
                        try:
                            score = float(text.rsplit("-", 1)[-1])
                        except ValueError:
                            score = float(body["query"].casefold() in text.casefold())
                    results.append({"index": i, "relevance_score": score})
                if case == "duplicate_index" and len(results) > 1:
                    results[-1]["index"] = 0
                elif case == "bad_index":
                    results[0]["index"] = len(documents)
                elif case == "bad_score":
                    results[0]["relevance_score"] = "invalid"
                elif case == "missing_row":
                    results.pop()
                response = {"results": list(reversed(results))}
            else:
                return self.reply({"error": "unknown endpoint"}, 404)
            if case == "http_error":
                return self.reply({"error": "synthetic service failure"}, 503)
            if case == "root_array":
                response = []
            elif case == "root_null":
                response = None
            elif case == "root_string":
                response = "invalid root"
            self.reply(response)
        except (KeyError, ValueError, TypeError, IndexError) as error:
            self.reply({"error": str(error)}, 400)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--default-dimension", type=int, default=6)
    args = parser.parse_args()
    if not 0 < args.default_dimension <= 16384:
        parser.error("default dimension must be between 1 and 16384")
    Handler.default_dimension = args.default_dimension
    ThreadingHTTPServer((args.bind, args.port), Handler).serve_forever()
