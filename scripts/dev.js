// Runs the backend API and the Vite dev server together.
'use strict';

const { spawn } = require('node:child_process');

const children = [
  spawn('node', ['--disable-warning=ExperimentalWarning', 'backend/server.js'], {
    stdio: 'inherit',
    env: process.env,
  }),
  spawn('npm', ['--prefix', 'frontend', 'run', 'dev'], {
    stdio: 'inherit',
    env: process.env,
  }),
];

function shutdown() {
  for (const child of children) {
    if (child && !child.killed) child.kill('SIGTERM');
  }
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
process.on('exit', shutdown);
