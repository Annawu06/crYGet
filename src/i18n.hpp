#pragma once

#include <array>
#include <map>
#include <string>

struct Language {
    const char* code;
    const char* name;
    const char* english_name;
};

inline constexpr std::array<Language, 11> languages{{
    {"en-US", "English", "English"}, {"zh-CN", "简体中文", "Chinese (Simplified)"},
    {"zh-HK", "繁體中文（香港）", "Chinese (Hong Kong)"},
    {"zh-TW", "繁體中文（台灣）", "Chinese (Traditional)"},
    {"fr", "Français", "French"},
    {"fr-CA", "Français (Canada)", "French (Canada)"}, {"ru", "Русский", "Russian"},
    {"es", "Español", "Spanish"}, {"es-419", "Español (Latinoamérica)", "Spanish (Latin America)"},
    {"pt-BR", "Português (Brasil)", "Portuguese (Brazil)"},
    {"pt-PT", "Português (Portugal)", "Portuguese (Portugal)"}
}};

using Messages = std::map<std::string, std::string>;

inline const Messages& messages(const std::string& locale) {
    static const std::map<std::string, Messages> catalogs{
        {"en-US", {
            {"paste", "Paste links"},
            {"hero", "Download YouTube videos"}, {"library", "Download list"},
            {"subtitle", "Paste video links, then click Download videos. Up to two videos download at once."},
            {"links", "Video links (one per line)"}, {"placeholder", "Paste links here · Ctrl+V"},
            {"add", "Download videos"}, {"quality", "Video quality"}, {"best", "Best available"},
            {"save", "Save to"}, {"hint", "Drag waiting videos to change download order."},
            {"empty", "No videos yet. Paste links above and click Download videos."}, {"queued", "Waiting · position"},
            {"connecting", "Connecting"}, {"downloading", "Downloading"}, {"canceling", "Canceling"},
            {"complete", "Downloaded"}, {"failed", "Download failed"}, {"canceled", "Canceled"},

            {"size", "Size"}, {"speed", "Speed"}, {"elapsed", "Elapsed"}, {"remaining", "Remaining"},
            {"open", "Open folder"}, {"logs", "Open logs folder"}, {"retry", "Retry download"}, {"cancel", "Cancel"}, {"remove", "Remove video"},
            {"invalid", "Use valid YouTube video links, up to 100 per batch."},
            {"folder_invalid", "Choose an existing save folder."}, {"duplicate", "These videos are already in the list."},
            {"added", "Downloads started"}, {"preview", "Loading preview…"},
            {"browse", "Browse"}, {"choose_folder", "Choose download folder"},
            {"up", "Up"}, {"use", "Use this folder"},
            {"active_count", "downloading"}, {"waiting_count", "waiting"}, {"done_count", "finished"}
        }},
        {"zh-CN", {
            {"paste", "粘贴链接"},
            {"hero", "下载 YouTube 视频"}, {"library", "下载列表"},
            {"subtitle", "粘贴视频链接，点击“下载视频”即可开始；最多同时下载两个。"},
            {"links", "视频链接（每行一个）"}, {"placeholder", "在此粘贴链接 · Ctrl+V"},
            {"add", "下载视频"}, {"quality", "视频画质"}, {"best", "最佳画质"},
            {"save", "保存到"}, {"hint", "拖动等待中的视频，可调整下载顺序。"},
            {"empty", "还没有视频。请粘贴链接并点击“下载视频”。"}, {"queued", "等待下载 · 第"},
            {"connecting", "正在连接"}, {"downloading", "正在下载"}, {"canceling", "正在取消"},
            {"complete", "下载完成"}, {"failed", "下载失败"}, {"canceled", "已取消"},

            {"size", "大小"}, {"speed", "速度"}, {"elapsed", "已用时间"}, {"remaining", "剩余时间"},
            {"open", "打开文件夹"}, {"logs", "打开日志目录"}, {"retry", "重新下载"}, {"cancel", "取消"}, {"remove", "移除视频"},
            {"invalid", "请输入有效的 YouTube 视频链接，每批最多 100 个。"},
            {"folder_invalid", "请选择已有的保存文件夹。"}, {"duplicate", "这些视频已在列表中。"},
            {"added", "已开始下载"}, {"preview", "正在加载预览…"},
            {"browse", "浏览"}, {"choose_folder", "选择下载文件夹"},
            {"up", "上一级"}, {"use", "使用此文件夹"},
            {"active_count", "下载中"}, {"waiting_count", "等待中"}, {"done_count", "已完成"}
        }},
        {"zh-HK", {
            {"paste", "貼上連結"},
            {"hero", "下載 YouTube 影片"}, {"library", "下載清單"},
            {"subtitle", "貼上影片連結，按「下載影片」即可開始；最多同時下載兩部。"},
            {"links", "影片連結（每行一個）"}, {"placeholder", "在此貼上連結 · Ctrl+V"},
            {"add", "下載影片"}, {"quality", "影片畫質"}, {"best", "最佳畫質"}, {"save", "儲存至"},
            {"hint", "拖動等候中的影片，可更改下載順序。"},
            {"empty", "尚無影片。請貼上連結並按「下載影片」。"}, {"queued", "等候下載 · 第"},
            {"connecting", "正在連接"}, {"downloading", "正在下載"}, {"canceling", "正在取消"},
            {"complete", "下載完成"}, {"failed", "下載失敗"}, {"canceled", "已取消"},

            {"size", "大小"}, {"speed", "速度"}, {"elapsed", "已用時間"}, {"remaining", "剩餘時間"},
            {"open", "開啟資料夾"}, {"logs", "開啟日誌資料夾"}, {"retry", "重新下載"}, {"cancel", "取消"}, {"remove", "移除影片"},
            {"invalid", "請輸入有效的 YouTube 影片連結，每批最多 100 個。"},
            {"folder_invalid", "請選擇現有的儲存資料夾。"}, {"duplicate", "這些影片已在清單中。"},
            {"added", "已開始下載"}, {"preview", "正在載入預覽…"},
            {"browse", "瀏覽"}, {"choose_folder", "選擇下載資料夾"},
            {"up", "上一級"}, {"use", "使用此資料夾"},
            {"active_count", "下載中"}, {"waiting_count", "等候中"}, {"done_count", "已完成"}
        }},
        {"zh-TW", {
            {"paste", "貼上連結"},
            {"hero", "下載 YouTube 影片"}, {"library", "下載清單"},
            {"subtitle", "貼上影片連結，按「下載影片」即可開始；最多同時下載兩部。"},
            {"links", "影片連結（每行一個）"}, {"placeholder", "在此貼上連結 · Ctrl+V"},
            {"add", "下載影片"}, {"quality", "影片畫質"}, {"best", "最佳畫質"}, {"save", "儲存至"},
            {"hint", "拖曳等待中的影片，可調整下載順序。"},
            {"empty", "尚無影片。請貼上連結並按「下載影片」。"}, {"queued", "等待下載 · 第"},
            {"connecting", "正在連線"}, {"downloading", "正在下載"}, {"canceling", "正在取消"},
            {"complete", "下載完成"}, {"failed", "下載失敗"}, {"canceled", "已取消"},

            {"size", "大小"}, {"speed", "速度"}, {"elapsed", "已用時間"}, {"remaining", "剩餘時間"},
            {"open", "開啟資料夾"}, {"logs", "開啟日誌資料夾"}, {"retry", "重新下載"}, {"cancel", "取消"}, {"remove", "移除影片"},
            {"invalid", "請輸入有效的 YouTube 影片連結，每批最多 100 個。"},
            {"folder_invalid", "請選擇現有的儲存資料夾。"}, {"duplicate", "這些影片已在清單中。"},
            {"added", "已開始下載"}, {"preview", "正在載入預覽…"},
            {"browse", "瀏覽"}, {"choose_folder", "選擇下載資料夾"},
            {"up", "上一層"}, {"use", "使用此資料夾"},
            {"active_count", "下載中"}, {"waiting_count", "等候中"}, {"done_count", "已完成"}
        }},
        {"fr", {
            {"paste", "Coller les liens"},
            {"hero", "Télécharger des vidéos YouTube"}, {"library", "Liste des téléchargements"},
            {"subtitle", "Collez les liens, puis cliquez sur Télécharger. Deux vidéos à la fois."},
            {"links", "Liens vidéo (un par ligne)"}, {"placeholder", "Collez les liens ici · Ctrl+V"},
            {"add", "Télécharger"}, {"quality", "Qualité vidéo"}, {"best", "Meilleure qualité"}, {"save", "Enregistrer dans"},
            {"hint", "Glissez les vidéos en attente pour changer l’ordre de téléchargement."},
            {"empty", "Aucune vidéo. Collez des liens et cliquez sur Télécharger."}, {"queued", "En attente · position"},
            {"connecting", "Connexion"}, {"downloading", "Téléchargement"}, {"canceling", "Annulation"},
            {"complete", "Téléchargée"}, {"failed", "Échec du téléchargement"}, {"canceled", "Annulée"},

            {"size", "Taille"}, {"speed", "Vitesse"}, {"elapsed", "Temps écoulé"}, {"remaining", "Temps restant"},
            {"open", "Ouvrir dossier"}, {"logs", "Ouvrir les journaux"}, {"retry", "Réessayer"}, {"cancel", "Annuler"}, {"remove", "Retirer la vidéo"},
            {"invalid", "Utilisez des liens YouTube valides, 100 maximum par lot."},
            {"folder_invalid", "Choisissez un dossier existant."}, {"duplicate", "Ces vidéos sont déjà dans la liste."},
            {"added", "Téléchargements lancés"}, {"preview", "Chargement de l’aperçu…"},
            {"browse", "Parcourir"}, {"choose_folder", "Choisir un dossier"},
            {"up", "Parent"}, {"use", "Utiliser ce dossier"},
            {"active_count", "en cours"}, {"waiting_count", "en attente"}, {"done_count", "terminées"}
        }},
        {"ru", {
            {"paste", "Вставить ссылки"},
            {"hero", "Скачать видео с YouTube"}, {"library", "Список загрузок"},
            {"subtitle", "Вставьте ссылки и нажмите «Скачать видео». Одновременно загружаются два видео."},
            {"links", "Ссылки на видео (по одной в строке)"}, {"placeholder", "Вставьте ссылки · Ctrl+V"},
            {"add", "Скачать видео"}, {"quality", "Качество видео"}, {"best", "Лучшее качество"}, {"save", "Сохранить в"},
            {"hint", "Перетащите ожидающие видео, чтобы изменить порядок загрузки."},
            {"empty", "Видео пока нет. Вставьте ссылки и нажмите «Скачать видео»."}, {"queued", "Ожидает · номер"},
            {"connecting", "Подключение"}, {"downloading", "Загрузка"}, {"canceling", "Отмена"},
            {"complete", "Загружено"}, {"failed", "Ошибка загрузки"}, {"canceled", "Отменено"},

            {"size", "Размер"}, {"speed", "Скорость"}, {"elapsed", "Прошло"}, {"remaining", "Осталось"},
            {"open", "Открыть папку"}, {"logs", "Открыть папку журналов"}, {"retry", "Повторить"}, {"cancel", "Отмена"}, {"remove", "Удалить из списка"},
            {"invalid", "Укажите корректные ссылки YouTube, до 100 за раз."},
            {"folder_invalid", "Выберите существующую папку."}, {"duplicate", "Эти видео уже в списке."},
            {"added", "Загрузка началась"}, {"preview", "Загрузка превью…"},
            {"browse", "Обзор"}, {"choose_folder", "Выберите папку"},
            {"up", "Выше"}, {"use", "Использовать папку"},
            {"active_count", "загружается"}, {"waiting_count", "ожидает"}, {"done_count", "готово"}
        }},
        {"es", {
            {"paste", "Pegar enlaces"},
            {"hero", "Descargar vídeos de YouTube"}, {"library", "Lista de descargas"},
            {"subtitle", "Pega los enlaces y pulsa Descargar vídeos. Se descargan dos a la vez."},
            {"links", "Enlaces de vídeo (uno por línea)"}, {"placeholder", "Pega enlaces aquí · Ctrl+V"},
            {"add", "Descargar vídeos"}, {"quality", "Calidad del vídeo"}, {"best", "Mejor calidad"}, {"save", "Guardar en"},
            {"hint", "Arrastra los vídeos en espera para cambiar el orden de descarga."},
            {"empty", "No hay vídeos. Pega enlaces y pulsa Descargar vídeos."}, {"queued", "En espera · puesto"},
            {"connecting", "Conectando"}, {"downloading", "Descargando"}, {"canceling", "Cancelando"},
            {"complete", "Descargado"}, {"failed", "Error de descarga"}, {"canceled", "Cancelado"},

            {"size", "Tamaño"}, {"speed", "Velocidad"}, {"elapsed", "Transcurrido"}, {"remaining", "Restante"},
            {"open", "Abrir carpeta"}, {"logs", "Abrir carpeta de registros"}, {"retry", "Reintentar"}, {"cancel", "Cancelar"}, {"remove", "Quitar vídeo"},
            {"invalid", "Usa enlaces válidos de YouTube, hasta 100 por lote."},
            {"folder_invalid", "Elige una carpeta existente."}, {"duplicate", "Estos vídeos ya están en la lista."},
            {"added", "Descargas iniciadas"}, {"preview", "Cargando vista previa…"},
            {"browse", "Explorar"}, {"choose_folder", "Elegir carpeta"},
            {"up", "Arriba"}, {"use", "Usar esta carpeta"},
            {"active_count", "descargando"}, {"waiting_count", "en espera"}, {"done_count", "terminadas"}
        }},
        {"pt-BR", {
            {"paste", "Colar links"},
            {"hero", "Baixar vídeos do YouTube"}, {"library", "Lista de downloads"},
            {"subtitle", "Cole os links e clique em Baixar vídeos. Dois vídeos baixam por vez."},
            {"links", "Links de vídeo (um por linha)"}, {"placeholder", "Cole links aqui · Ctrl+V"},
            {"add", "Baixar vídeos"}, {"quality", "Qualidade do vídeo"}, {"best", "Melhor qualidade"}, {"save", "Salvar em"},
            {"hint", "Arraste os vídeos em espera para mudar a ordem de download."},
            {"empty", "Não há vídeos. Cole os links e clique em Baixar vídeos."}, {"queued", "Em espera · posição"},
            {"connecting", "Conectando"}, {"downloading", "Baixando"}, {"canceling", "Cancelando"},
            {"complete", "Baixado"}, {"failed", "Falha no download"}, {"canceled", "Cancelado"},

            {"size", "Tamanho"}, {"speed", "Velocidade"}, {"elapsed", "Decorrido"}, {"remaining", "Restante"},
            {"open", "Abrir pasta"}, {"logs", "Abrir pasta de registros"}, {"retry", "Tentar novamente"}, {"cancel", "Cancelar"}, {"remove", "Remover vídeo"},
            {"invalid", "Use links válidos do YouTube, até 100 por lote."},
            {"folder_invalid", "Escolha uma pasta existente."}, {"duplicate", "Esses vídeos já estão na lista."},
            {"added", "Downloads iniciados"}, {"preview", "Carregando prévia…"},
            {"browse", "Procurar"}, {"choose_folder", "Escolher pasta"},
            {"up", "Acima"}, {"use", "Usar esta pasta"},
            {"active_count", "baixando"}, {"waiting_count", "aguardando"}, {"done_count", "concluídos"}
        }},
        {"pt-PT", {
            {"paste", "Colar ligações"},
            {"hero", "Transferir vídeos do YouTube"}, {"library", "Lista de transferências"},
            {"subtitle", "Cole as ligações e clique em Transferir vídeos. Duas transferências de cada vez."},
            {"links", "Ligações dos vídeos (uma por linha)"}, {"placeholder", "Cole ligações aqui · Ctrl+V"},
            {"add", "Transferir vídeos"}, {"quality", "Qualidade do vídeo"}, {"best", "Melhor qualidade"}, {"save", "Guardar em"},
            {"hint", "Arraste os vídeos em espera para mudar a ordem de transferência."},
            {"empty", "Não há vídeos. Cole as ligações e clique em Transferir vídeos."}, {"queued", "Em espera · posição"},
            {"connecting", "A ligar"}, {"downloading", "A transferir"}, {"canceling", "A cancelar"},
            {"complete", "Transferido"}, {"failed", "Falha na transferência"}, {"canceled", "Cancelado"},

            {"size", "Tamanho"}, {"speed", "Velocidade"}, {"elapsed", "Decorrido"}, {"remaining", "Restante"},
            {"open", "Abrir pasta"}, {"logs", "Abrir pasta de registos"}, {"retry", "Tentar novamente"}, {"cancel", "Cancelar"}, {"remove", "Remover vídeo"},
            {"invalid", "Use ligações válidas do YouTube, até 100 por lote."},
            {"folder_invalid", "Escolha uma pasta existente."}, {"duplicate", "Estes vídeos já estão na lista."},
            {"added", "Transferências iniciadas"}, {"preview", "A carregar pré-visualização…"},
            {"browse", "Procurar"}, {"choose_folder", "Escolher pasta"},
            {"up", "Acima"}, {"use", "Utilizar esta pasta"},
            {"active_count", "a transferir"}, {"waiting_count", "em espera"}, {"done_count", "concluídas"}
        }}
    };
    std::string base = locale;
    if (base == "fr-CA") base = "fr";
    if (base == "es-419") base = "es";
    auto found = catalogs.find(base);
    return found == catalogs.end() ? catalogs.at("en-US") : found->second;
}

inline std::string tr(const std::string& locale, const std::string& key) {
    const auto& catalog = messages(locale);
    auto found = catalog.find(key);
    if (found != catalog.end()) return found->second;
    return messages("en-US").at(key);
}
