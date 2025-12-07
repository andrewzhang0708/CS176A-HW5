#!/bin/bash
# submit.sh — package and push your HW3 files automatically

# Exit immediately on errors
set -e

# Add and commit changes
echo "Committing and pushing to GitHub..."
git add .
git commit -m "client"
git push

echo "Submission complete!"

