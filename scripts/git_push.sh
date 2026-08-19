#!/bin/bash

PROJECT="Morgana"

rm -rf uplogs

echo "git add ."
git add .

echo "git commit -m \"$1\""
git commit -m "$1"

echo "git push origin main"
git push origin main

echo

echo "git clone git@github.com:devlucasfs/uplogs.git"
git clone git@github.com:devlucasfs/uplogs.git

echo "cd uplogs"
cd uplogs || exit

tmp=$(mktemp)

cat README.md > "$tmp"

cat >> "$tmp" <<EOF
# New commit in $PROJECT.
- Date: $(date "+%Y-%m-%d %H:%M:%S")
\`\`\`
$1
\`\`\`
EOF

mv "$tmp" README.md

echo "git add ."
git add .

echo "git commit -m \"update($PROJECT): $1\""
git commit -m "update($PROJECT): $1"

echo "git push origin main"
git push origin main

echo "rm -rf uplogs"
rm -rf uplogs
