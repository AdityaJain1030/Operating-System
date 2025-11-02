import os

dir = 'hard_docs'
docs_path = 'docs.txt'

if os.path.exists(dir):
    for filename in os.listdir(dir):
        file_path = os.path.join(dir, filename)
        os.remove(file_path)
    os.rmdir(dir)
os.mkdir(dir)

with open(docs_path, 'r') as f:
    docs_content = f.read()

for i in range(51):
    o = os.path.join(dir, f"{i}.txt")
    content = f"{i}\n{docs_content}"
    
    with open(o, 'w') as f:
        f.write(content)

print("Files created successfully.")
