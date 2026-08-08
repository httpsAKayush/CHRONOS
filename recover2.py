import json

path = "/home/zer0/.gemini/antigravity/brain/76714e1a-c6b4-4ea5-a630-0979da913fac/.system_generated/logs/transcript.jsonl"
res = ""

with open(path, "r") as f:
    for line in f:
        line = line.strip()
        if not line: continue
        try:
            data = json.loads(line)
            if data.get("type") == "TOOL_RESPONSE" and "int cmdDiagnose" in data.get("content", ""):
                res += data.get("content") + "\n\n"
        except Exception as e:
            pass

with open("cmdDiagnose_recovery.txt", "w") as out:
    out.write(res)
