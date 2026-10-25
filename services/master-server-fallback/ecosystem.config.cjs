// pm2: `pm2 start ecosystem.config.cjs`, then `pm2 save` so it starts with
// the machine (README.md).
module.exports = {
  apps: [{
    name: 'legobattles-master-fallback',
    script: 'server.mjs',
    cwd: __dirname,
    interpreter: 'node',
    autorestart: true,
    restart_delay: 5000,
    max_restarts: 50,
    // workerd is a child process; pm2 stops it with the app.
    kill_timeout: 8000,
    out_file: 'logs/out.log',
    error_file: 'logs/error.log',
    merge_logs: true,
    time: true,
  }],
};
