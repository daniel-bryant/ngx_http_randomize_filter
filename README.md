# ngx_http_randomize_filter

## Installation

```
git clone https://github.com/nginx/nginx.git
cd nginx/

./auto/configure --add-module=/path/to/ngx_http_randomize_filter/ --with-debug
make && sudo make install && make clean

sudo /usr/local/nginx/sbin/nginx
```

## Usage
```
# nginx.conf
location / {
    randomize_filter 'bannerAd';
    randomize_filter_salt 'foobar';
    randomize_filter_once off;
    randomize_filter_types *;
}
```

## Stopping
```
sudo /usr/local/nginx/sbin/nginx -s quit
```
