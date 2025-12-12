# MPMC Dev Image Kullanım Kılavuzu

## Amaç
C++20 MPMC (Multiple Producer Multiple Consumer) circular buffer örneğini geliştirmek, derlemek ve çalıştırmak için hazırlanmış Docker geliştirme imajı ve konteyneri.

## Dockerfile Açıklamaları
- `FROM ubuntu:24.04` — Ubuntu 24.04 taban imajı.
- `RUN apt-get update ... build-essential gdb cmake ninja-build clang-format git vim nano curl ca-certificates ...` — Derleyici, debugger, build araçları, formatlayıcı, editörler ve sertifika paketleri yüklenir.
- `ARG USERNAME=dev` — Oluşturulacak kullanıcı adı.
- `ARG USER_UID=1001` — Kullanıcı UID (host UID 1000 ile çakışmaması için 1001 seçildi).
- `ARG USER_GID=1001` — Kullanıcı GID (aynı gerekçeyle 1001).
- `RUN (getent group ... || groupadd ...) && (id -u ... || useradd ...)` — Grup yoksa oluşturur, kullanıcı yoksa ekler; home dizinini oluşturur.
- `WORKDIR /workspace` — Varsayılan çalışma dizini.
- `CMD ["sleep", "infinity"]` — Konteyneri etkileşimli geliştirme için ayakta tutar. (İsterseniz `docker run ... /bin/bash` ile kabuk açabilirsiniz.)

## İmaj Oluşturma
```bash
cd /home/user/works/MPMC
docker build -t mpmc-dev .
```

## Konteyner Çalıştırma
### Docker Compose ile
```bash
cd /home/user/works/MPMC
docker compose up -d
docker compose exec mpmc-dev /bin/bash   # konteynere attach olmak için
```

### Docker run ile (alternatif)
- Root (mount izin sorunlarını aşmak için en sorunsuz yöntem):
```bash
docker run --rm -it --user root -v /home/user/works/MPMC:/workspace mpmc-dev /bin/bash
```
- `dev` kullanıcısıyla (host izinleri uygunsa):
```bash
docker run --rm -it --user dev -v /home/user/works/MPMC:/workspace mpmc-dev /bin/bash
```

## Derleme ve Çalıştırma (konteyner içi)
```bash
g++ -std=c++20 -O2 -pthread main.cpp -o app
./app
```

### CMake ile Derleme (konteyner içi)
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/app
```

## Docker İzin Sorunu (permission denied /var/run/docker.sock)
Eğer `docker ps` veya `docker compose` komutlarında `permission denied ... /var/run/docker.sock` hatası alırsanız:
```bash
sudo groupadd docker         # grup yoksa; varsa "already exists" uyarısı normal
sudo usermod -aG docker $USER
newgrp docker                # yeni shell'de docker grubuna geçmek için
docker ps                    # erişimi doğrula
```
Not: Yeni terminal açarsanız tekrar `newgrp docker` yapın veya docker grubuyla açılmış bir oturum kullanın.

## Test Suite Çalıştırma
Kapsamlı test suite'i çalıştırmak için:
```bash
cd /workspace/MPMC
# Test dosyası (test.cpp) zaten local'de mevcut
g++ -std=c++20 -O2 -pthread test.cpp -o test_app
./test_app
```

**Local'de çalıştırma:**
```bash
cd /home/user/works/MPMC/MPMC
g++ -std=c++20 -O2 -pthread test.cpp -o test_app
./test_app
```

Test suite şu testleri içerir:
1. **test_basic_producer_consumer**: Temel producer/consumer işlevselliği
2. **test_non_blocking**: Veri yoksa nullopt dönmesi
3. **test_exception_safety**: Producer fail olsa bile slot kaybolmaması (tail_ commit'te artırılıyor)
4. **test_multiple_producer_consumer**: Çoklu thread producer/consumer
5. **test_shutdown**: Shutdown sonrası nullopt dönmesi
6. **test_capacity_limit**: Buffer dolu olduğunda nullopt dönmesi
7. **test_thread_safety**: Thread safety (race condition testi)
8. **test_commit_increments_tail**: Commit sonrası tail artışı

## Notlar ve İzinler
- Host dosyaları UID/GID 1000 ise `--user root` ile derlemek yazma izin sorunlarını önler. İsterseniz hostta `chown -R 1001:1001 /home/user/works/MPMC` yapıp `--user dev` ile çalışabilirsiniz.
- Docker daemon erişimi için WSL'de `docker` grubunun etkin olduğu bir oturumda olun (`newgrp docker` veya yeni terminal).

## Permission Denied Sorunu (WSL)
Docker container içinde root olarak oluşturulan dosyalar host'ta da root sahipliğinde kalır. Bu yüzden `user` kullanıcısı yazamaz.

**Çözüm:**
```bash
# Klasör sahipliğini düzelt
sudo chown -R user:user /home/user/works/MPMC/MPMC

# Veya tüm MPMC klasörü için
sudo chown -R user:user /home/user/works/MPMC
```

**Önleme:** Container içinde dosya oluştururken `--user dev` kullanın veya host'ta dosya oluşturun.

