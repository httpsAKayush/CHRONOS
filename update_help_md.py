import re

with open('src/cli/main_cli.cpp', 'r') as f:
    content = f.read()

# Extract the help string
match = re.search(r'printColoredHelp\(R"\((.*?)\)"\);', content, re.DOTALL)
if match:
    help_text = match.group(1)
    # Strip all @XY@ tags
    clean_text = re.sub(r'@[A-Z]{2}@', '', help_text)
    
    # Strip leading newline if exists
    if clean_text.startswith('\n'):
        clean_text = clean_text[1:]
        
    with open('help.md', 'w') as f:
        f.write("```text\n")
        f.write(clean_text)
        f.write("```\n")
    print("help.md updated.")
else:
    print("Could not find help text in main_cli.cpp")
