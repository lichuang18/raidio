import matplotlib.pyplot as plt
import pandas as pd
import sys
import math

def plot_bandwidth(filename, plot_type):
    # print(plot_type)
    if plot_type not in ['bw', 'iops', 'lat']:
        print(f"Unknown plot_type: {plot_type}")
        sys.exit(1)

    df = pd.read_csv(filename, header=None,
                     names=['rw', 'qd', 'bs_kb', 'num_threads', 'bw_mb'])

    for rw_type in ['read', 'write']:
        df_rw = df[df['rw'] == rw_type]

        num_threads_list = sorted(df_rw['num_threads'].unique())
        bs_list = sorted(df_rw['bs_kb'].unique())

        n = len(num_threads_list)
        rows = math.ceil(n / 2)
        cols = 2 if n > 1 else 1

        fig, axs = plt.subplots(nrows=rows, ncols=cols, figsize=(12, 6))
        axs = axs.flatten() if n > 1 else [axs]

        colors = plt.cm.tab10.colors

        for i, num_threads in enumerate(num_threads_list):
            ax = axs[i]
            df_thread = df_rw[df_rw['num_threads'] == num_threads]
            for j, bs in enumerate(bs_list):
                df_bs = df_thread[df_thread['bs_kb'] == bs]
                qd_labels = df_bs['qd'].astype(str).tolist()
                x = range(len(qd_labels))
                if plot_type == 'bw':
                    y = df_bs['bw_mb'].tolist()
                elif plot_type == 'iops':
                    y_ = df_bs['bw_mb'].tolist()
                    y = [float(v * 1024 / bs) if v != 0 else 0 for v in y_]
                elif plot_type == 'lat':
                    # todo list
                    y = df_bs['bw_mb'].tolist()
                # y_ = df_bs['bw_mb'].tolist()
                # y = [float(v * 1024 / bs) if v != 0 else 0 for v in y_]
                ax.plot(x, y, marker='o', label=f"{bs} KB", color=colors[j % len(colors)])

            ax.set_title(f"Threads = {num_threads}", fontsize=11)
            ax.set_xlabel("Queue Depth (QD)")
            if plot_type == 'bw':
                ax.set_ylabel("Bandwidth (MB/s)")
            elif plot_type == 'iops':
                ax.set_ylabel("IOPS")
            elif plot_type == 'lat':
                ax.set_ylabel("latency us")
            # ax.set_ylabel("Bandwidth (MB/s)")
            ax.set_xticks(x)
            ax.set_xticklabels(qd_labels)

        # 全局标题，往下压得更近
        if plot_type == 'bw':
            fig.suptitle(f"{rw_type.upper()} Bandwidth vs QD", fontsize=14, y=0.94)
        elif plot_type == 'iops':
            fig.suptitle(f"{rw_type.upper()} IOPS vs QD", fontsize=14, y=0.94)
        elif plot_type == 'lat':
            fig.suptitle(f"{rw_type.upper()} latency vs QD", fontsize=14, y=0.94)
        # fig.suptitle(f"{rw_type.upper()} Bandwidth vs QD", fontsize=14, y=0.94)

        # 共用图例放底部中间
        handles, labels = axs[0].get_legend_handles_labels()
        fig.legend(handles, labels, loc='lower center', ncol=len(bs_list), bbox_to_anchor=(0.5, 0.01))

        # 只调整子图位置，不动标题和图例
        fig.subplots_adjust(top=0.86, bottom=0.16, hspace=0.35)
        # if plot_type == 'bw':
        #     plt.savefig(f"result/{rw_type}_bw_plot.png", dpi=300)
        # elif plot_type == 'iops':
        #     plt.savefig(f"result/{rw_type}_iops_plot.png", dpi=300)
        # elif plot_type == 'lat':
        #     plt.savefig(f"result/{rw_type}_lat_plot.png", dpi=300)
        plt.savefig(f"result/{rw_type}_{plot_type}_plot.png", dpi=300)
        plt.show()

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python plot_bw.py <data_file.csv> <plot_type>")
        sys.exit(1)
    plot_bandwidth(sys.argv[1],sys.argv[2])
