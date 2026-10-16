# Host-side tests

The firmware verifies on-device, but the web UI's field↔JSON contract can be
checked on a laptop. `web_ui_contract.test.js` loads the editor page embedded in
`ChoreStation/web_server.h` with jsdom, feeds it the real seed files, exercises
render → add → collect → save, and asserts the JSON keys the page emits match
what `storage_manager.h` actually reads (so a renamed field can't silently break
saves).

```sh
cd test
npm init -y && npm i jsdom
node web_ui_contract.test.js
```

Exit code 0 = all pass. `node_modules/` is gitignored.
