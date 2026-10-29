var espbloxconfig = {
  hostname: "HOSTNAME_HERE",
  port: "PORT_HERE"
}
// instructions: replace HOSTNAME_HERE with your public ip and PORT_HERE with the port you forwarded from your router
export default {
  async fetch(request) {
    const url = new URL(request.url);
    url.hostname = espbloxconfig.hostname;
    url.port = espbloxconfig.port;
    return fetch(new Request(url, request));
  }
};