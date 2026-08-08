import json

path = "/home/zer0/.gemini/antigravity/brain/76714e1a-c6b4-4ea5-a630-0979da913fac/.system_generated/logs/transcript_full.jsonl"
res = ""

with open(path, "r") as f:
    for line in f:
        line = line.strip()
        if not line: continue
        try:
            data = json.loads(line)
            if "main_cli.cpp" in json.dumps(data):
                res += json.dumps(data) + "\n\n"
        except Exception as e:
            pass

with open("recovered.txt", "w") as out:
    out.write(res)
